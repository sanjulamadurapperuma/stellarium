/*
 * Copyright (C) 2009, 2012 Matthew Gates
 * Copyright (C) 2015 Nick Fedoseev (Iridium flares)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelProjector.hpp"
#include "StelPainter.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelGui.hpp"
#include "StelGuiItems.hpp"
#include "StelLocation.hpp"
#include "StelObjectMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelLocaleMgr.hpp"
#include "StelFileMgr.hpp"
#include "StelTextureMgr.hpp"
#include "StelIniParser.hpp"
#include "Satellites.hpp"
#include "Satellite.hpp"
#include "SatellitesListModel.hpp"
#include "Planet.hpp"
#include "SolarSystem.hpp"
#include "StelJsonParser.hpp"
#include "SatellitesDialog.hpp"
#include "LabelMgr.hpp"
#include "StelTranslator.hpp"
#include "StelProgressController.hpp"
#include "StelUtils.hpp"

#include "external/qtcompress/qzipreader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QKeyEvent>
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QTimer>
#include <QVariantMap>
#include <QVariant>
#include <QDir>
#include <QTemporaryFile>

StelModule* SatellitesStelPluginInterface::getStelModule() const
{
	return new Satellites();
}

StelPluginInfo SatellitesStelPluginInterface::getPluginInfo() const
{
	// Allow to load the resources when used as a static plugin
	Q_INIT_RESOURCE(Satellites);

	StelPluginInfo info;
	info.id = "Satellites";
	info.displayedName = N_("Satellites");
	info.authors = "Matthew Gates, Jose Luis Canales";
	info.contact = "https://github.com/Stellarium/stellarium";
	info.description = N_("Prediction of artificial satellite positions in Earth orbit based on NORAD TLE data");
	info.version = SATELLITES_PLUGIN_VERSION;
	info.license = SATELLITES_PLUGIN_LICENSE;
	return info;
}

Satellites::Satellites()
	: satelliteListModel(Q_NULLPTR)
	, toolbarButton(Q_NULLPTR)
	, earth(Q_NULLPTR)
	, defaultHintColor(0.0f, 0.4f, 0.6f)
	, defaultOrbitColor(0.0f, 0.3f, 0.6f)
	, updateState(CompleteNoUpdates)
	, downloadMgr(Q_NULLPTR)
	, progressBar(Q_NULLPTR)
	, numberDownloadsComplete(0)
	, updateTimer(Q_NULLPTR)
	, updatesEnabled(false)
	, autoAddEnabled(false)
	, autoRemoveEnabled(false)
	, updateFrequencyHours(0)
	, iridiumFlaresPredictionDepth(7)
{
	setObjectName("Satellites");
	configDialog = new SatellitesDialog();
}

void Satellites::deinit()
{
	Satellite::hintTexture.clear();
	texPointer.clear();
}

Satellites::~Satellites()
{
	delete configDialog;
}


void Satellites::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();

	try
	{
		// TODO: Compatibility with installation-dir modules? --BM
		// It seems that the original code couldn't handle them either.
		QString dirPath = StelFileMgr::getUserDir() + "/modules/Satellites";
		// TODO: Ideally, this should return a QDir object
		StelFileMgr::makeSureDirExistsAndIsWritable(dirPath);
		dataDir.setPath(dirPath);

		// If no settings in the main config file, create with defaults
		if (!conf->childGroups().contains("Satellites"))
		{
			//qDebug() << "Stellites: created section in config file.";
			restoreDefaultSettings();
		}

		// populate settings from main config file.
		loadSettings();

		// absolute file name for inner catalog of the satellites
		catalogPath = dataDir.absoluteFilePath("satellites.json");
		// absolute file name for qs.mag file
		qsMagFilePath = dataDir.absoluteFilePath("qs.mag");

		// Load and find resources used in the plugin
		texPointer = StelApp::getInstance().getTextureManager().createTexture(StelFileMgr::getInstallationDir()+"/textures/pointeur5.png");
		Satellite::hintTexture = StelApp::getInstance().getTextureManager().createTexture(":/satellites/hint.png");

		// key bindings and other actions		
		QString satGroup = N_("Satellites");
		addAction("actionShow_Satellite_Hints", satGroup, N_("Satellite hints"), "hintsVisible", "Ctrl+Z");
		addAction("actionShow_Satellite_Labels", satGroup, N_("Satellite labels"), "labelsVisible", "Alt+Shift+Z");
		addAction("actionShow_Satellite_ConfigDialog_Global", satGroup, N_("Satellites configuration window"), configDialog, "visible", "Alt+Z");

		// Gui toolbar button
		StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
		if (gui!=Q_NULLPTR)
		{
			toolbarButton = new StelButton(Q_NULLPTR,
						       QPixmap(":/satellites/bt_satellites_on.png"),
						       QPixmap(":/satellites/bt_satellites_off.png"),
						       QPixmap(":/graphicGui/glow32x32.png"),
						       "actionShow_Satellite_Hints");
			gui->getButtonBar()->addButton(toolbarButton, "065-pluginsGroup");
		}
	}
	catch (std::runtime_error &e)
	{
		qWarning() << "[Satellites] init error: " << e.what();
		return;
	}

	// If the json file does not already exist, create it from the resource in the QT resource
	if(QFileInfo(catalogPath).exists())
	{
		if (!checkJsonFileFormat() || readCatalogVersion() != SATELLITES_PLUGIN_VERSION)
		{
			displayMessage(q_("The old satellites.json file is no longer compatible - using default file"), "#bb0000");
			restoreDefaultCatalog();
		}
	}
	else
	{
		qDebug() << "[Satellites] satellites.json does not exist - copying default file to " << QDir::toNativeSeparators(catalogPath);
		restoreDefaultCatalog();
	}
	
	if(!QFileInfo(qsMagFilePath).exists())
	{
		restoreDefaultQSMagFile();
	}

	qDebug() << "[Satellites] loading catalog file:" << QDir::toNativeSeparators(catalogPath);

	// create satellites according to content os satellites.json file
	loadCatalog();

	// Set up download manager and the update schedule
	downloadMgr = new QNetworkAccessManager(this);
	connect(downloadMgr, SIGNAL(finished(QNetworkReply*)),
	        this, SLOT(saveDownloadedUpdate(QNetworkReply*)));
	updateState = CompleteNoUpdates;
	updateTimer = new QTimer(this);
	updateTimer->setSingleShot(false);   // recurring check for update
	updateTimer->setInterval(13000);     // check once every 13 seconds to see if it is time for an update
	connect(updateTimer, SIGNAL(timeout()), this, SLOT(checkForUpdate()));
	updateTimer->start();

	earth = GETSTELMODULE(SolarSystem)->getEarth();
	GETSTELMODULE(StelObjectMgr)->registerStelObjectMgr(this);

	// Handle changes to the observer location or wide range of dates:
	StelCore* core = StelApp::getInstance().getCore();
	connect(core, SIGNAL(locationChanged(StelLocation)), this, SLOT(updateObserverLocation(StelLocation)));
	
	// let sat symbols stay on-screen even if highly unprecise over time 
	//connect(core, SIGNAL(dateChangedForMonth()), this, SLOT(updateSatellitesVisibility()));
	//connect(core, SIGNAL(dateChangedByYear()), this, SLOT(updateSatellitesVisibility()));
}

void Satellites::updateSatellitesVisibility()
{
	if (getFlagHints())
		setFlagHints(false);
}

bool Satellites::backupCatalog(bool deleteOriginal)
{
	QFile old(catalogPath);
	if (!old.exists())
	{
		qWarning() << "[Satellites] no file to backup";
		return false;
	}

	QString backupPath = catalogPath + ".old";
	if (QFileInfo(backupPath).exists())
		QFile(backupPath).remove();

	if (old.copy(backupPath))
	{
		if (deleteOriginal)
		{
			if (!old.remove())
			{
				qWarning() << "[Satellites] WARNING: unable to remove old catalog file!";
				return false;
			}
		}
	}
	else
	{
		qWarning() << "[Satellites] WARNING: failed to back up catalog file as"
			   << QDir::toNativeSeparators(backupPath);
		return false;
	}

	return true;
}

double Satellites::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName==StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("SolarSystem")->getCallOrder(actionName)+1.;
	return 0;
}

QList<StelObjectP> Satellites::searchAround(const Vec3d& av, double limitFov, const StelCore* core) const
{
	QList<StelObjectP> result;
	if (!hintFader)
		return result;

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return result;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return result;

	Vec3d v(av);
	v.normalize();
	double cosLimFov = cos(limitFov * M_PI/180.);
	Vec3d equPos;

	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->displayed)
		{
			equPos = sat->XYZ;
			equPos.normalize();
			if (equPos[0]*v[0] + equPos[1]*v[1] + equPos[2]*v[2]>=cosLimFov)
			{
				result.append(qSharedPointerCast<StelObject>(sat));
			}
		}
	}
	return result;
}

StelObjectP Satellites::searchByNameI18n(const QString& nameI18n) const
{
	if (!hintFader)
		return Q_NULLPTR;

	StelCore* core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return Q_NULLPTR;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return Q_NULLPTR;
	
	QString objw = nameI18n.toUpper();
	
	StelObjectP result = searchByNoradNumber(objw);
	if (result)
		return result;

	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->displayed)
		{
			if (sat->getNameI18n().toUpper() == objw)
				return qSharedPointerCast<StelObject>(sat);
		}
	}

	return Q_NULLPTR;
}

StelObjectP Satellites::searchByName(const QString& englishName) const
{
	if (!hintFader)
		return Q_NULLPTR;

	StelCore* core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return Q_NULLPTR;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return Q_NULLPTR;

	QString objw = englishName.toUpper();
	
	StelObjectP result = searchByNoradNumber(objw);
	if (result)
		return result;
	
	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->displayed)
		{
			if (sat->getEnglishName().toUpper() == objw)
				return qSharedPointerCast<StelObject>(sat);
		}
	}

	return Q_NULLPTR;
}

StelObjectP Satellites::searchByID(const QString &id) const
{
	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->getID() == id)
		{
			return qSharedPointerCast<StelObject>(sat);
		}
	}

	return Q_NULLPTR;
}

StelObjectP Satellites::searchByNoradNumber(const QString &noradNumber) const
{
	if (!hintFader)
		return Q_NULLPTR;

	StelCore* core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return Q_NULLPTR;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return Q_NULLPTR;
	
	// If the search string is a catalog number...
	QRegExp regExp("^(NORAD)\\s*(\\d+)\\s*$");
	if (regExp.exactMatch(noradNumber))
	{
		QString numberString = regExp.capturedTexts().at(2);
		
		for (const auto& sat : satellites)
		{
			if (sat->initialized && sat->displayed)
			{
				if (sat->getCatalogNumberString() == numberString)
					return qSharedPointerCast<StelObject>(sat);
			}
		}
	}
	
	return StelObjectP();
}

QStringList Satellites::listMatchingObjects(const QString& objPrefix, int maxNbItem, bool useStartOfWords, bool inEnglish) const
{
	QStringList result;
	if (!hintFader || maxNbItem <= 0)
		return result;

	StelCore* core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return result;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return result;

	QString objw = objPrefix.toUpper();

	QString numberPrefix;
	QRegExp regExp("^(NORAD)\\s*(\\d+)\\s*$");
	if (regExp.exactMatch(objw))
	{
		QString numberString = regExp.capturedTexts().at(2);
		bool ok;
		/* int number = */ numberString.toInt(&ok);
		if (ok)
			numberPrefix = numberString;
	}

	for (const auto& sobj : satellites)
	{
		if (!sobj->initialized || !sobj->displayed)
		{
			continue;
		}

		QString name = inEnglish ? sobj->getEnglishName() : sobj->getNameI18n();
		if (matchObjectName(name, objPrefix, useStartOfWords))
		{
			result.append(name);
		}
		else if (!numberPrefix.isEmpty() && sobj->getCatalogNumberString().startsWith(numberPrefix))
		{
			result.append(QString("NORAD %1").arg(sobj->getCatalogNumberString()));
		}

		if (result.size() >= maxNbItem)
		{
			break;
		}
	}

	result.sort();
	return result;
}

QStringList Satellites::listAllObjects(bool inEnglish) const
{
	QStringList result;

	if (!hintFader)
		return result;

	StelCore* core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return result;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return result;

	if (inEnglish)
	{
		for (const auto& sat : satellites)
		{
			result << sat->getEnglishName();
		}
	}
	else
	{
		for (const auto& sat : satellites)
		{
			result << sat->getNameI18n();
		}
	}
	return result;
}

bool Satellites::configureGui(bool show)
{
	if (show)
		configDialog->setVisible(true);
	return true;
}

void Satellites::restoreDefaults(void)
{
	restoreDefaultSettings();
	restoreDefaultCatalog();
	restoreDefaultQSMagFile();
	loadCatalog();
	loadSettings();
}

void Satellites::restoreDefaultSettings()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->beginGroup("Satellites");

	// delete all existing Satellite settings...
	conf->remove("");

	conf->setValue("show_satellite_hints", true);
	conf->setValue("show_satellite_labels", false);
	conf->setValue("updates_enabled", true);
	conf->setValue("auto_add_enabled", true);
	conf->setValue("auto_remove_enabled", true);
	conf->setValue("hint_color", "0.0,0.4,0.6");
	conf->setValue("invisible_satellite_color", "0.2,0.2,0.2");
	conf->setValue("hint_font_size", 10);
	conf->setValue("update_frequency_hours", 72);
	conf->setValue("orbit_line_flag", false);
	conf->setValue("orbit_line_segments", 90);
	conf->setValue("orbit_fade_segments", 5);
	conf->setValue("orbit_segment_duration", 20);
	conf->setValue("realistic_mode_enabled", true);
	
	conf->endGroup(); // saveTleSources() opens it for itself
	
	// TLE update sources
	QStringList urls;
	urls << "1,http://www.celestrak.com/NORAD/elements/visual.txt" // Auto-add ON!
	     << "http://www.celestrak.com/NORAD/elements/tle-new.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/science.txt"
	     << "http://www.celestrak.com/NORAD/elements/noaa.txt"
	     << "http://www.celestrak.com/NORAD/elements/goes.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/amateur.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/gps-ops.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/galileo.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/iridium.txt"
	     << "http://www.celestrak.com/NORAD/elements/geo.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/stations.txt"
	     << "http://www.celestrak.com/NORAD/elements/weather.txt"
	     << "http://www.celestrak.com/NORAD/elements/resource.txt"
	     << "http://www.celestrak.com/NORAD/elements/sarsat.txt"
	     << "http://www.celestrak.com/NORAD/elements/dmc.txt"
	     << "http://www.celestrak.com/NORAD/elements/tdrss.txt"
	     << "http://www.celestrak.com/NORAD/elements/argos.txt"
	     << "http://www.celestrak.com/NORAD/elements/intelsat.txt"
	     << "http://www.celestrak.com/NORAD/elements/gorizont.txt"
	     << "http://www.celestrak.com/NORAD/elements/raduga.txt"
	     << "http://www.celestrak.com/NORAD/elements/molniya.txt"
	     << "http://www.celestrak.com/NORAD/elements/orbcomm.txt"
	     << "http://www.celestrak.com/NORAD/elements/globalstar.txt"
	     << "http://www.celestrak.com/NORAD/elements/x-comm.txt"
	     << "http://www.celestrak.com/NORAD/elements/other-comm.txt"
	     << "1,http://www.celestrak.com/NORAD/elements/glo-ops.txt"
	     << "http://www.celestrak.com/NORAD/elements/beidou.txt"
	     << "http://www.celestrak.com/NORAD/elements/sbas.txt"
	     << "http://www.celestrak.com/NORAD/elements/nnss.txt"
	     << "http://www.celestrak.com/NORAD/elements/engineering.txt"
	     << "http://www.celestrak.com/NORAD/elements/education.txt"
	     << "http://www.celestrak.com/NORAD/elements/geodetic.txt"
	     << "http://www.celestrak.com/NORAD/elements/radar.txt"
	     << "http://www.celestrak.com/NORAD/elements/cubesat.txt"
	     << "http://www.celestrak.com/NORAD/elements/other.txt"
	     << "https://www.amsat.org/amsat/ftp/keps/current/nasabare.txt"
	     << "1,https://www.prismnet.com/~mmccants/tles/classfd.zip";

	saveTleSources(urls);
}

void Satellites::restoreDefaultCatalog()
{
	if (QFileInfo(catalogPath).exists())
		backupCatalog(true);

	QFile src(":/satellites/satellites.json");
	if (!src.copy(catalogPath))
	{
		qWarning() << "[Satellites] cannot copy json resource to " + QDir::toNativeSeparators(catalogPath);
	}
	else
	{
		qDebug() << "[Satellites] copied default satellites.json to " << QDir::toNativeSeparators(catalogPath);
		// The resource is read only, and the new file inherits this...  make sure the new file
		// is writable by the Stellarium process so that updates can be done.
		QFile dest(catalogPath);
		dest.setPermissions(dest.permissions() | QFile::WriteOwner);

		// Make sure that in the case where an online update has previously been done, but
		// the json file has been manually removed, that an update is schreduled in a timely
		// manner
		StelApp::getInstance().getSettings()->remove("Satellites/last_update");
		lastUpdate = QDateTime::fromString("2015-05-01T12:00:00", Qt::ISODate);
	}
}

void Satellites::restoreDefaultQSMagFile()
{
	QFile src(":/satellites/qs.mag");
	if (!src.copy(qsMagFilePath))
	{
		qWarning() << "[Satellites] cannot copy qs.mag resource to " + QDir::toNativeSeparators(qsMagFilePath);
	}
	else
	{
		qDebug() << "[Satellites] copied default qs.mag to " << QDir::toNativeSeparators(qsMagFilePath);
		// The resource is read only, and the new file inherits this...  make sure the new file
		// is writable by the Stellarium process so that updates can be done.
		QFile dest(qsMagFilePath);
		dest.setPermissions(dest.permissions() | QFile::WriteOwner);
	}
}

void Satellites::loadSettings()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->beginGroup("Satellites");

	// Load update sources list...
	updateUrls.clear();
	
	// Backward compatibility: try to detect and read an old-stlye array.
	// TODO: Assume that the user hasn't modified their conf in a stupid way?
//	if (conf->contains("tle_url0")) // This can skip some operations...
	QRegExp keyRE("^tle_url\\d+$");
	QStringList urls;
	for (const auto& key : conf->childKeys())
	{
		if (keyRE.exactMatch(key))
		{
			QString url = conf->value(key).toString();
			conf->remove(key); // Delete old-style keys
			if (url.isEmpty())
				continue;
			// NOTE: This URL is also hardcoded in restoreDefaultSettings().
			if (url == "http://celestrak.com/NORAD/elements/visual.txt")
				url.prepend("1,"); // Same as in the new default configuration
			urls << url;
		}
	}
	// If any have been read, save them in the new format.
	if (!urls.isEmpty())
	{
		conf->endGroup();
		setTleSources(urls);
		conf->beginGroup("Satellites");
	}
	else
	{
		int size = conf->beginReadArray("tle_sources");
		for (int i = 0; i < size; i++)
		{
			conf->setArrayIndex(i);
			QString url = conf->value("url").toString();
			if (!url.isEmpty())
			{
				if (conf->value("add_new").toBool())
					url.prepend("1,");
				updateUrls.append(url);
			}
		}
		conf->endArray();
	}
	
	// NOTE: Providing default values AND using restoreDefaultSettings() to create the section seems redundant. --BM 
	
	// updater related settings...
	updateFrequencyHours = conf->value("update_frequency_hours", 72).toInt();
	// last update default is the first Towell Day.  <3 DA
	lastUpdate = QDateTime::fromString(conf->value("last_update", "2001-05-25T12:00:00").toString(), Qt::ISODate);
	setFlagHints(conf->value("show_satellite_hints", true).toBool());
	Satellite::showLabels = conf->value("show_satellite_labels", false).toBool();
	updatesEnabled = conf->value("updates_enabled", true).toBool();
	autoAddEnabled = conf->value("auto_add_enabled", true).toBool();
	autoRemoveEnabled = conf->value("auto_remove_enabled", true).toBool();
	iridiumFlaresPredictionDepth = conf->value("flares_prediction_depth", 7).toInt();

	// Get a font for labels
	labelFont.setPixelSize(conf->value("hint_font_size", 10).toInt());

	// orbit drawing params
	Satellite::orbitLinesFlag = conf->value("orbit_line_flag", false).toBool();
	Satellite::orbitLineSegments = conf->value("orbit_line_segments", 90).toInt();
	Satellite::orbitLineFadeSegments = conf->value("orbit_fade_segments", 5).toInt();
	Satellite::orbitLineSegmentDuration = conf->value("orbit_segment_duration", 20).toInt();

	Satellite::invisibleSatelliteColor = StelUtils::strToVec3f(conf->value("invisible_satellite_color", "0.2,0.2,0.2").toString());

	Satellite::timeRateLimit = conf->value("time_rate_limit", 1.0).toDouble();

	// realistic mode
	setFlagRelisticMode(conf->value("realistic_mode_enabled", true).toBool());
	setFlagHideInvisibleSatellites(conf->value("hide_invisible_satellites", false).toBool());

	conf->endGroup();
}

void Satellites::saveSettings()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->beginGroup("Satellites");

	// updater related settings...
	conf->setValue("update_frequency_hours", updateFrequencyHours);
	conf->setValue("show_satellite_hints", getFlagHints());
	conf->setValue("show_satellite_labels", Satellite::showLabels);
	conf->setValue("updates_enabled", updatesEnabled );
	conf->setValue("auto_add_enabled", autoAddEnabled);
	conf->setValue("auto_remove_enabled", autoRemoveEnabled);
	conf->setValue("flares_prediction_depth", iridiumFlaresPredictionDepth);

	// Get a font for labels
	conf->setValue("hint_font_size", labelFont.pixelSize());

	// orbit drawing params
	conf->setValue("orbit_line_flag", Satellite::orbitLinesFlag);
	conf->setValue("orbit_line_segments", Satellite::orbitLineSegments);
	conf->setValue("orbit_fade_segments", Satellite::orbitLineFadeSegments);
	conf->setValue("orbit_segment_duration", Satellite::orbitLineSegmentDuration);

	// realistic mode
	conf->setValue("realistic_mode_enabled", getFlagRealisticMode());
	conf->setValue("hide_invisible_satellites", getFlagHideInvisibleSatellites());

	conf->endGroup();
	
	// Update sources...
	saveTleSources(updateUrls);
}

void Satellites::loadCatalog()
{
	setDataMap(loadDataMap());
}

const QString Satellites::readCatalogVersion()
{
	QString jsonVersion("unknown");
	QFile satelliteJsonFile(catalogPath);
	if (!satelliteJsonFile.open(QIODevice::ReadOnly))
	{
		qWarning() << "[Satellites] cannot open " << QDir::toNativeSeparators(catalogPath);
		return jsonVersion;
	}

	QVariantMap map;
	try
	{
		map = StelJsonParser::parse(&satelliteJsonFile).toMap();
		satelliteJsonFile.close();
	}
	catch (std::runtime_error &e)
	{
		qDebug() << "[Satellites] File format is wrong! Error: " << e.what();
		return jsonVersion;
	}

	if (map.contains("creator"))
	{
		QString creator = map.value("creator").toString();
		QRegExp vRx(".*(\\d+\\.\\d+\\.\\d+).*");
		if (vRx.exactMatch(creator))
		{
			jsonVersion = vRx.capturedTexts().at(1);
		}
	}

	//qDebug() << "[Satellites] catalog version from file:" << jsonVersion;
	return jsonVersion;
}

bool Satellites::saveDataMap(const QVariantMap& map, QString path)
{
	if (path.isEmpty())
		path = catalogPath;

	QFile jsonFile(path);

	if (jsonFile.exists())
		jsonFile.remove();

	if (!jsonFile.open(QIODevice::WriteOnly))
	{
		qWarning() << "[Satellites] cannot open for writing:" << QDir::toNativeSeparators(path);
		return false;
	}
	else
	{
		qDebug() << "[Satellites] writing to:" << QDir::toNativeSeparators(path);
		StelJsonParser::write(map, &jsonFile);
		jsonFile.close();
		return true;
	}
}

QVariantMap Satellites::loadDataMap(QString path)
{
	if (path.isEmpty())
		path = catalogPath;

	QVariantMap map;
	QFile jsonFile(path);
	if (!jsonFile.open(QIODevice::ReadOnly))
		qWarning() << "[Satellites] cannot open " << QDir::toNativeSeparators(path);
	else
	{
		try
		{
			map = StelJsonParser::parse(&jsonFile).toMap();
			jsonFile.close();
		}
		catch (std::runtime_error &e)
		{
			qDebug() << "[Satellites] File format is wrong! Error: " << e.what();
			return QVariantMap();
		}
	}
	return map;
}

void Satellites::setDataMap(const QVariantMap& map)
{
	int numReadOk = 0;
	QVariantList defaultHintColorMap;
	defaultHintColorMap << defaultHintColor[0] << defaultHintColor[1] << defaultHintColor[2];

	if (map.contains("hintColor"))
	{
		defaultHintColorMap = map.value("hintColor").toList();
		defaultHintColor.set(defaultHintColorMap.at(0).toFloat(), defaultHintColorMap.at(1).toFloat(), defaultHintColorMap.at(2).toFloat());
	}

	if (satelliteListModel)
		satelliteListModel->beginSatellitesChange();
	
	satellites.clear();
	groups.clear();
	QVariantMap satMap = map.value("satellites").toMap();
	for (const auto& satId : satMap.keys())
	{
		QVariantMap satData = satMap.value(satId).toMap();

		if (!satData.contains("hintColor"))
			satData["hintColor"] = defaultHintColorMap;

		if (!satData.contains("orbitColor"))
			satData["orbitColor"] = satData["hintColor"];

		if (!satData.contains("stdMag") && qsMagList.contains(satId))
			satData["stdMag"] = qsMagList[satId];

		SatelliteP sat(new Satellite(satId, satData));
		if (sat->initialized)
		{
			satellites.append(sat);
			groups.unite(sat->groups);
			numReadOk++;
		}
	}
	std::sort(satellites.begin(), satellites.end());
	
	if (satelliteListModel)
		satelliteListModel->endSatellitesChange();
}

QVariantMap Satellites::createDataMap(void)
{
	QVariantMap map;
	QVariantList defHintCol;
	defHintCol << Satellite::roundToDp(defaultHintColor[0],3)
		   << Satellite::roundToDp(defaultHintColor[1],3)
		   << Satellite::roundToDp(defaultHintColor[2],3);

	map["creator"] = QString("Satellites plugin version %1 (updated)").arg(SATELLITES_PLUGIN_VERSION);
	map["hintColor"] = defHintCol;
	map["shortName"] = "satellite orbital data";
	QVariantMap sats;
	for (const auto& sat : satellites)
	{
		QVariantMap satMap = sat->getMap();

		if (satMap["orbitColor"] == satMap["hintColor"])
			satMap.remove("orbitColor");

		if (satMap["hintColor"].toList() == defHintCol)
			satMap.remove("hintColor");

		if (satMap["stdMag"].toFloat() == 99.f)
			satMap.remove("stdMag");

		if (satMap["status"].toInt() == Satellite::StatusUnknown)
			satMap.remove("status");

		sats[sat->id] = satMap;		
	}
	map["satellites"] = sats;
	return map;
}

void Satellites::markLastUpdate()
{
	lastUpdate = QDateTime::currentDateTime();
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->setValue("Satellites/last_update", lastUpdate.toString(Qt::ISODate));
}

QSet<QString> Satellites::getGroups() const
{
	return groups;
}

QStringList Satellites::getGroupIdList() const
{
	QStringList groupList(groups.values());
	groupList.sort();
	return groupList;
}

void Satellites::addGroup(const QString& groupId)
{
	if (groupId.isEmpty())
		return;
	groups.insert(groupId);
}

QHash<QString,QString> Satellites::getSatellites(const QString& group, Status vis) const
{
	QHash<QString,QString> result;

	for (const auto& sat : satellites)
	{
		if (sat->initialized)
		{
			if ((group.isEmpty() || sat->groups.contains(group)) && ! result.contains(sat->id))
			{
				if (vis==Both ||
				   (vis==Visible && sat->displayed) ||
				   (vis==NotVisible && !sat->displayed) ||
				   (vis==OrbitError && !sat->orbitValid) ||
				   (vis==NewlyAdded && sat->isNew()))
					result.insert(sat->id, sat->name);
			}
		}
	}
	return result;
}

SatellitesListModel* Satellites::getSatellitesListModel()
{
	if (!satelliteListModel)
		satelliteListModel = new SatellitesListModel(&satellites, this);
	return satelliteListModel;
}

SatelliteP Satellites::getById(const QString& id) const
{
	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->id == id)
			return sat;
	}
	return SatelliteP();
}

QStringList Satellites::listAllIds() const
{
	QStringList result;
	for (const auto& sat : satellites)
	{
		if (sat->initialized)
			result.append(sat->id);
	}
	return result;
}

bool Satellites::add(const TleData& tleData)
{
	//TODO: Duplicates check!!! --BM
	
	// More validation?
	if (tleData.id.isEmpty() ||
	        tleData.name.isEmpty() ||
	        tleData.first.isEmpty() ||
	        tleData.second.isEmpty())
		return false;
	
	QVariantList hintColor;
	hintColor << defaultHintColor[0]
	          << defaultHintColor[1]
	          << defaultHintColor[2];
	
	QVariantMap satProperties;
	satProperties.insert("name", tleData.name);
	satProperties.insert("tle1", tleData.first);
	satProperties.insert("tle2", tleData.second);
	satProperties.insert("hintColor", hintColor);
	//TODO: Decide if newly added satellites are visible by default --BM
	satProperties.insert("visible", true);
	satProperties.insert("orbitVisible", false);
	if (qsMagList.contains(tleData.id))
		satProperties.insert("stdMag", qsMagList[tleData.id]);
	if (tleData.status != Satellite::StatusUnknown)
		satProperties.insert("status", tleData.status);
	
	SatelliteP sat(new Satellite(tleData.id, satProperties));
	if (sat->initialized)
	{
		qDebug() << "[Satellites] satellite added:" << tleData.id << tleData.name;
		satellites.append(sat);
		sat->setNew();
		return true;
	}
	return false;
}

void Satellites::add(const TleDataList& newSatellites)
{
	if (satelliteListModel)
		satelliteListModel->beginSatellitesChange();
	
	int numAdded = 0;
	for (const auto& tleSet : newSatellites)
	{
		if (add(tleSet))
		{
			numAdded++;
		}
	}
	if (numAdded > 0)
		std::sort(satellites.begin(), satellites.end());
	
	if (satelliteListModel)
		satelliteListModel->endSatellitesChange();
	
	qDebug() << "[Satellites] "
		 << newSatellites.count() << "satellites proposed for addition, "
		 << numAdded << " added, "
		 << satellites.count() << " total after the operation.";
}

void Satellites::remove(const QStringList& idList)
{
	if (satelliteListModel)
		satelliteListModel->beginSatellitesChange();
	
	StelObjectMgr* objMgr = GETSTELMODULE(StelObjectMgr);
	int numRemoved = 0;
	for (int i = 0; i < satellites.size(); i++)
	{
		const SatelliteP& sat = satellites.at(i);
		if (idList.contains(sat->id))
		{
			QList<StelObjectP> selected = objMgr->getSelectedObject("Satellite");
			if (selected.contains(sat.staticCast<StelObject>()))
				objMgr->unSelect();
			
			qDebug() << "Satellite removed:" << sat->id << sat->name;
			satellites.removeAt(i);
			i--; //Compensate for the change in the array's indexing
			numRemoved++;
		}
	}
	// As the satellite list is kept sorted, no need for re-sorting.
	
	if (satelliteListModel)
		satelliteListModel->endSatellitesChange();

	qDebug() << "[Satellites] "
		 << idList.count() << "satellites proposed for removal, "
		 << numRemoved << " removed, "
		 << satellites.count() << " remain.";
}

int Satellites::getSecondsToUpdate(void)
{
	QDateTime nextUpdate = lastUpdate.addSecs(updateFrequencyHours * 3600);
	return QDateTime::currentDateTime().secsTo(nextUpdate);
}

void Satellites::setTleSources(QStringList tleSources)
{
	updateUrls = tleSources;
	saveTleSources(updateUrls);
}

void Satellites::saveTleSources(const QStringList& urls)
{
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->beginGroup("Satellites");

	// clear old source list
	conf->remove("tle_sources");

	int index = 0;
	conf->beginWriteArray("tle_sources");
	for (auto url : urls)
	{
		conf->setArrayIndex(index++);
		if (url.startsWith("1,"))
		{
			conf->setValue("add_new", true);
			url.remove(0, 2);
		}
		else if (url.startsWith("0,"))
			url.remove(0, 2);
		conf->setValue("url", url);
	}
	conf->endArray();

	conf->endGroup();
}

bool Satellites::getFlagLabels() const
{
	return Satellite::showLabels;
}

void Satellites::enableInternetUpdates(bool enabled)
{
	if (enabled != updatesEnabled)
	{
		updatesEnabled = enabled;
		emit settingsChanged();
	}
}

void Satellites::enableAutoAdd(bool enabled)
{
	if (autoAddEnabled != enabled)
	{
		autoAddEnabled = enabled;
		emit settingsChanged();
	}
}

void Satellites::enableAutoRemove(bool enabled)
{
	if (autoRemoveEnabled != enabled)
	{
		autoRemoveEnabled = enabled;
		emit settingsChanged();
	}
}

bool Satellites::getFlagRealisticMode() const
{
	return Satellite::realisticModeFlag;
}

bool Satellites::getFlagHideInvisibleSatellites() const
{
	return Satellite::hideInvisibleSatellitesFlag;
}

void Satellites::setFlagRelisticMode(bool b)
{
	if (Satellite::realisticModeFlag != b)
	{
		Satellite::realisticModeFlag = b;
		emit settingsChanged();
	}
}

void Satellites::setFlagHideInvisibleSatellites(bool b)
{
	if (Satellite::hideInvisibleSatellitesFlag != b)
	{
		Satellite::hideInvisibleSatellitesFlag = b;
		emit settingsChanged();
	}
}

void Satellites::setFlagHints(bool b)
{
	if (hintFader != b)
	{
		hintFader = b;
		emit settingsChanged(); // GZ IS THIS REQUIRED/USEFUL??
		emit hintsVisibleChanged(b);
	}
}

void Satellites::setFlagLabels(bool b)
{
	if (Satellite::showLabels != b)
	{
		Satellite::showLabels = b;
		emit settingsChanged(); // GZ IS THIS REQUIRED/USEFUL??
		emit labelsVisibleChanged(b);
	}
}

void Satellites::setLabelFontSize(int size)
{
	if (labelFont.pixelSize() != size)
	{
		labelFont.setPixelSize(size);
		emit settingsChanged();
	}
}

void Satellites::setUpdateFrequencyHours(int hours)
{
	if (updateFrequencyHours != hours)
	{
		updateFrequencyHours = hours;
		emit settingsChanged();
	}
}

void Satellites::checkForUpdate(void)
{
	if (updatesEnabled && (updateState != Updating)
	    && (lastUpdate.addSecs(updateFrequencyHours * 3600) <= QDateTime::currentDateTime()))
	{
		updateFromOnlineSources();
	}
}

void Satellites::updateFromOnlineSources()
{
	// never update TLE's for any date before Oct 4, 1957, 19:28:34GMT ;-)
	if (StelApp::getInstance().getCore()->getJD()<2436116.3115)
		return;

	if (updateState==Satellites::Updating)
	{
		qWarning() << "[Satellites] Internet update already in progress!";
		return;
	}
	else
	{
		qDebug() << "[Satellites] starting Internet update...";
	}

	// Setting lastUpdate should be done only when the update is finished. -BM

	// TODO: Perhaps tie the emptyness of updateUrls to updatesEnabled... --BM
	if (updateUrls.isEmpty())
	{
		qWarning() << "[Satellites] update failed."
		           << "No update sources are defined!";
		
		// Prevent from re-entering this method on the next check:
		markLastUpdate();
		// TODO: Do something saner, such as disabling internet updates,
		// or stopping the timer. --BM
		emit updateStateChanged(OtherError);
		emit tleUpdateComplete(0, satellites.count(), 0, 0);
		return;
	}

	updateState = Satellites::Updating;
	emit(updateStateChanged(updateState));
	updateSources.clear();
	numberDownloadsComplete = 0;

	if (progressBar==Q_NULLPTR)
		progressBar = StelApp::getInstance().addProgressBar();

	progressBar->setValue(0);
	progressBar->setRange(0, updateUrls.size());
	progressBar->setFormat("TLE download %v/%m");

	for (auto url : updateUrls)
	{
		TleSource source;
		source.file = 0;
		source.addNew = false;
		if (url.startsWith("1,"))
		{
			// Also prevents inconsistent behavior if the user toggles the flag
			// while an update is in progress.
			source.addNew = autoAddEnabled;
			url.remove(0, 2);
		}
		else if (url.startsWith("0,"))
			url.remove(0, 2);
		
		source.url.setUrl(url);
		if (source.url.isValid())
		{
			updateSources.append(source);
			downloadMgr->get(QNetworkRequest(source.url));
		}
	}
}

void Satellites::saveDownloadedUpdate(QNetworkReply* reply)
{
	// check the download worked, and save the data to file if this is the case.
	if (reply->error() == QNetworkReply::NoError && reply->bytesAvailable()>0)
	{
		// download completed successfully.
		QString name = QString("tle%1.txt").arg(numberDownloadsComplete);
		QString path = dataDir.absoluteFilePath(name);
		// QFile as a child object to the plugin to ease memory management
		QFile* tmpFile = new QFile(path, this);
		if (tmpFile->exists())
			tmpFile->remove();

		if (tmpFile->open(QIODevice::WriteOnly | QIODevice::Text))
		{
			QByteArray fd = reply->readAll();
			// qWarning() << "[Satellites] Processing an URL:" << reply->url().toString();
			if (reply->url().toString().contains(".zip", Qt::CaseInsensitive))
			{
				QTemporaryFile zip;
				if (zip.open())
				{
					// qWarning() << "[Satellites] Processing a ZIP archive...";
					zip.write(fd);
					zip.close();
					QString archive = zip.fileName();
					QByteArray data;

					Stel::QZipReader reader(archive);
					if (reader.status() != Stel::QZipReader::NoError)
						qWarning() << "[Satellites] Unable to open as a ZIP archive";
					else
					{
						QList<Stel::QZipReader::FileInfo> infoList = reader.fileInfoList();
						for (const auto& info : infoList)
						{
							// qWarning() << "[Satellites] Processing:" << info.filePath;
							if (info.isFile)
								data.append(reader.fileData(info.filePath));
						}
						// qWarning() << "[Satellites] Extracted data:" << data;
						fd = data;
					}
					reader.close();
					zip.remove();
				}
				else
					qWarning() << "[Satellites] Unable to open a temporary file";
			}
			tmpFile->write(fd);
			tmpFile->close();
			
			// The reply URL can be different form the requested one...
			QUrl url = reply->request().url();
			for (int i = 0; i < updateSources.count(); i++)
			{
				if (updateSources[i].url == url)
				{
					updateSources[i].file = tmpFile;
					tmpFile = 0;
					break;
				}
			}
			if (tmpFile) // Something strange just happened...
				delete tmpFile; // ...so we have to clean.
		}
		else
		{
			qWarning() << "[Satellites] cannot save update file:"
			           << tmpFile->error()
			           << tmpFile->errorString();
		}
	}
	else
		qWarning() << "[Satellites] FAILED to download" << reply->url().toString(QUrl::RemoveUserInfo) << "Error:" << reply->errorString();

	numberDownloadsComplete++;
	if (progressBar)
		progressBar->setValue(numberDownloadsComplete);

	// Check if all files have been downloaded.
	// TODO: It's better to keep track of the network requests themselves. --BM 
	if (numberDownloadsComplete < updateSources.size())
		return;
	
	if (progressBar)
	{
		StelApp::getInstance().removeProgressBar(progressBar);
		progressBar = Q_NULLPTR;
	}
	
	// All files have been downloaded, finish the update
	TleDataHash newData;
	for (int i = 0; i < updateSources.count(); i++)
	{
		if (!updateSources[i].file)
			continue;
		if (updateSources[i].file->open(QFile::ReadOnly|QFile::Text))
		{
			parseTleFile(*updateSources[i].file,
			             newData,
			             updateSources[i].addNew);
			updateSources[i].file->close();
			delete updateSources[i].file;
			updateSources[i].file = 0;
		}
	}
	updateSources.clear();	
	parseQSMagFile(qsMagFilePath);
	updateSatellites(newData);
}

void Satellites::updateObserverLocation(const StelLocation &loc)
{
	Q_UNUSED(loc)
	recalculateOrbitLines();
}

void Satellites::setFlagOrbitLines(bool b)
{
	Satellite::orbitLinesFlag = b;
}

bool Satellites::getFlagOrbitLines() const
{
	return Satellite::orbitLinesFlag;
}

void Satellites::recalculateOrbitLines(void)
{
	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->displayed && sat->orbitDisplayed)
			sat->recalculateOrbitLines();
	}
}

void Satellites::displayMessage(const QString& message, const QString hexColor)
{
	messageIDs << GETSTELMODULE(LabelMgr)->labelScreen(message, 30, 30 + (20*messageIDs.count()), true, 16, hexColor, false, 9000);
}


void Satellites::saveCatalog(QString path)
{
	saveDataMap(createDataMap(), path);
}

void Satellites::updateFromFiles(QStringList paths, bool deleteFiles)
{
	// Container for the new data.
	TleDataHash newTleSets;
	for (const auto& tleFilePath : paths)
	{
		QFile tleFile(tleFilePath);
		if (tleFile.open(QIODevice::ReadOnly|QIODevice::Text))
		{
			parseTleFile(tleFile, newTleSets, autoAddEnabled);
			tleFile.close();

			if (deleteFiles)
				tleFile.remove();
		}
	}
	parseQSMagFile(qsMagFilePath);
	updateSatellites(newTleSets);
}

void Satellites::updateSatellites(TleDataHash& newTleSets)
{
	// Save the update time.
	// One of the reasons it's here is that lastUpdate is used below.
	markLastUpdate();
	
	if (newTleSets.isEmpty())
	{
		qWarning() << "[Satellites] update files contain no TLE sets!";
		updateState = OtherError;
		emit(updateStateChanged(updateState));
		return;
	}
	
	if (satelliteListModel)
		satelliteListModel->beginSatellitesChange();
	
	// Right, we should now have a map of all the elements we downloaded.  For each satellite
	// which this module is managing, see if it exists with an updated element, and update it if so...
	int sourceCount = newTleSets.count(); // newTleSets is modified below
	int updatedCount = 0;
	int totalCount = 0;
	int addedCount = 0;
	int missingCount = 0; // Also the number of removed sats, if any.
	QStringList toBeRemoved;
	for (const auto& sat : satellites)
	{
		totalCount++;
		
		// Satellites marked as "user-defined" are protected from updates and
		// removal.
		if (sat->userDefined)
		{
			qDebug() << "Satellite ignored (user-protected):"
			         << sat->id << sat->name;
			continue;
		}
		
		QString id = sat->id;
		TleData newTle = newTleSets.take(id);
		if (!newTle.name.isEmpty())
		{
			if (sat->tleElements.first != newTle.first ||
			    sat->tleElements.second != newTle.second ||
			    sat->name != newTle.name)
			{
				// We have updated TLE elements for this satellite
				sat->setNewTleElements(newTle.first, newTle.second);
				
				// Update the name if it has been changed in the source list
				sat->name = newTle.name;

				// Update operational status
				sat->status = newTle.status;

				// we reset this to "now" when we started the update.
				sat->lastUpdated = lastUpdate;
				updatedCount++;
			}
			if (qsMagList.contains(id))
				sat->stdMag = qsMagList[id];
		}
		else
		{
			if (autoRemoveEnabled)
				toBeRemoved.append(sat->id);
			else
				qWarning() << "[Satellites]" << sat->id << sat->name
				           << "is missing in the update lists.";
			missingCount++;
		}
		// All satellites, who has invalid orbit should be removed.
		if (!sat->orbitValid)
		{
			toBeRemoved.append(sat->id);
			qWarning() << "[Satellites]" << sat->id << sat->name
				   << "has invalid orbit and will be removed.";
			missingCount++;
		}
	}
	
	// Only those not in the loaded collection have remained
	// (autoAddEnabled is not checked, because it's already in the flags)
	for (const auto& tleData : newTleSets)
	{
		if (tleData.addThis)
		{
			// Add the satellite...
			if (add(tleData))
				addedCount++;
		}
	}
	if (addedCount)
		std::sort(satellites.begin(), satellites.end());
	
	if (autoRemoveEnabled && !toBeRemoved.isEmpty())
	{
		qWarning() << "[Satellites] purging objects that were not updated...";
		remove(toBeRemoved);
	}
	
	if (updatedCount > 0 ||
	        (autoRemoveEnabled && missingCount > 0))
	{
		saveDataMap(createDataMap());
		updateState = CompleteUpdates;
	}
	else
		updateState = CompleteNoUpdates;
	
	if (satelliteListModel)
		satelliteListModel->endSatellitesChange();

	qDebug() << "[Satellites] update finished."
	         << updatedCount << "/" << totalCount << "updated,"
	         << addedCount << "added,"
	         << missingCount << "missing or removed."
	         << sourceCount << "source entries parsed.";

	emit(updateStateChanged(updateState));
	emit(tleUpdateComplete(updatedCount, totalCount, addedCount, missingCount));
}

void Satellites::parseTleFile(QFile& openFile,
                              TleDataHash& tleList,
                              bool addFlagValue)
{
	if (!openFile.isOpen() || !openFile.isReadable())
		return;
	
	// Code mostly re-used from updateFromFiles()
	int lineNumber = 0;
	TleData lastData;
	lastData.addThis = addFlagValue;
	
	while (!openFile.atEnd())
	{
		QString line = QString(openFile.readLine()).trimmed();
		if (line.length() < 65) // this is title line
		{
			// New entry in the list, so reset all fields
			lastData = TleData();
			lastData.addThis = addFlagValue;
			
			// The thing in square brackets after the name is actually
			// Celestrak's "status code". Parse it!
			QStringList codes;
			codes << "+" << "-" << "P" << "B" << "S" << "X" << "D" << "?";

			QRegExp statusRx("\\s*\\[(\\D{1})\\]\\s*$");
			statusRx.setMinimal(true);
			if (statusRx.indexIn(line)>-1)
			{
				lastData.status = Satellite::StatusUnknown;
				switch (codes.indexOf(statusRx.capturedTexts().at(1).toUpper()))
				{
					case 0:
						lastData.status = Satellite::StatusOperational;
						break;
					case 1:
						lastData.status = Satellite::StatusNonoperational;
						break;
					case 2:
						lastData.status = Satellite::StatusPartiallyOperational;
						break;
					case 3:
						lastData.status = Satellite::StatusStandby;
						break;
					case 4:
						lastData.status = Satellite::StatusSpare;
						break;
					case 5:
						lastData.status = Satellite::StatusExtendedMission;
						break;
					case 6:
						lastData.status = Satellite::StatusDecayed;
						break;
					default:
						lastData.status = Satellite::StatusUnknown;
				}
			}

			//TODO: We need to think of some kind of ecaping these
			//characters in the JSON parser. --BM
			line.replace(QRegExp("\\s*\\[([^\\]])*\\]\\s*$"),"");  // remove "status code" from name
			lastData.name = line;
		}
		else
		{
			// TODO: Yet another place suitable for a standard TLE regex. --BM
			if (QRegExp("^1 .*").exactMatch(line))
				lastData.first = line;
			else if (QRegExp("^2 .*").exactMatch(line))
			{
				lastData.second = line;
				// The Satellite Catalog Number is the second number
				// on the second line.
				QString id = line.split(' ').at(1).trimmed();
				if (id.isEmpty())
					continue;
				lastData.id = id;
				
				// This is the second line and there will be no more,
				// so if everything is OK, save the elements.
				if (!lastData.name.isEmpty() &&	!lastData.first.isEmpty())
				{
					// Some satellites can be listed in multiple files,
					// and only some of those files may be marked for adding,
					// so try to preserve the flag - if it's set,
					// feel free to overwrite the existing value.
					// If not, overwrite only if it's not in the list already.
					// NOTE: Second case overwrite may need to check which TLE set is newer. 
					if (lastData.addThis || !tleList.contains(id))
						tleList.insert(id, lastData); // Overwrite if necessary
				}
				//TODO: Error warnings? --BM
			}
			else
				qDebug() << "[Satellites] unprocessed line " << lineNumber <<  " in file " << QDir::toNativeSeparators(openFile.fileName());
		}
	}
}

void Satellites::parseQSMagFile(QString qsMagFile)
{
	// Description of file and some additional information you can find here:
	// 1) http://www.prismnet.com/~mmccants/tles/mccdesc.html
	// 2) http://www.prismnet.com/~mmccants/tles/intrmagdef.html
	if (qsMagFile.isEmpty())
		return;

	QFile qsmFile(qsMagFile);
	if (!qsmFile.open(QIODevice::ReadOnly))
	{
		qWarning() << "[Satellites] oops... cannot open " << QDir::toNativeSeparators(qsMagFile);
		return;
	}

	qsMagList.clear();
	while (!qsmFile.atEnd())
	{
		QString line = QString(qsmFile.readLine());
		QString id   = line.mid(0,5).trimmed();
		QString smag = line.mid(33,4).trimmed();
		if (!smag.isEmpty())
			qsMagList.insert(id, smag.toDouble());
	}
	qsmFile.close();
}

void Satellites::update(double deltaTime)
{
	// Separated because first test should be very fast.
	if (!hintFader && hintFader.getInterstate() <= 0.)
		return;

	StelCore *core = StelApp::getInstance().getCore();

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return;

	if (core->getCurrentPlanet() != earth || !isValidRangeDates(core))
		return;

	hintFader.update((int)(deltaTime*1000));

	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->displayed)
			sat->update(deltaTime);
	}
}

void Satellites::draw(StelCore* core)
{
	// Separated because first test should be very fast.
	if (!hintFader && hintFader.getInterstate() <= 0.)
		return;

	if (qAbs(core->getTimeRate())>=Satellite::timeRateLimit) // Do not show satellites when time rate is over limit
		return;

	if (core->getCurrentPlanet()!=earth || !isValidRangeDates(core))
		return;

	StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	StelPainter painter(prj);
	painter.setFont(labelFont);
	Satellite::hintBrightness = hintFader.getInterstate();

	painter.setBlending(true);
	Satellite::hintTexture->bind();
	Satellite::viewportHalfspace = painter.getProjector()->getBoundingCap();
	for (const auto& sat : satellites)
	{
		if (sat && sat->initialized && sat->displayed)
			sat->draw(core, painter);
	}

	if (GETSTELMODULE(StelObjectMgr)->getFlagSelectedObjectPointer())
		drawPointer(core, painter);
}

void Satellites::drawPointer(StelCore* core, StelPainter& painter)
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);

	const QList<StelObjectP> newSelected = GETSTELMODULE(StelObjectMgr)->getSelectedObject("Satellite");
	if (!newSelected.empty())
	{
		const StelObjectP obj = newSelected[0];
		Vec3d pos=obj->getJ2000EquatorialPos(core);
		Vec3d screenpos;

		// Compute 2D pos and return if outside screen
		if (!prj->project(pos, screenpos))
			return;
		painter.setColor(0.4f,0.5f,0.8f);
		texPointer->bind();

		painter.setBlending(true);

		// Size on screen
		float size = obj->getAngularSize(core)*M_PI/180.*prj->getPixelPerRadAtCenter();
		size += 12.f + 3.f*std::sin(2.f * StelApp::getInstance().getTotalRunTime());
		// size+=20.f + 10.f*std::sin(2.f * StelApp::getInstance().getTotalRunTime());
		painter.drawSprite2dMode(screenpos[0]-size/2, screenpos[1]-size/2, 20, 90);
		painter.drawSprite2dMode(screenpos[0]-size/2, screenpos[1]+size/2, 20, 0);
		painter.drawSprite2dMode(screenpos[0]+size/2, screenpos[1]+size/2, 20, -90);
		painter.drawSprite2dMode(screenpos[0]+size/2, screenpos[1]-size/2, 20, -180);
	}
}

bool Satellites::checkJsonFileFormat()
{
	QFile jsonFile(catalogPath);
	if (!jsonFile.open(QIODevice::ReadOnly))
	{
		qWarning() << "[Satellites] cannot open " << QDir::toNativeSeparators(catalogPath);
		return false;
	}

	QVariantMap map;
	try
	{
		map = StelJsonParser::parse(&jsonFile).toMap();
		jsonFile.close();
	}
	catch (std::runtime_error& e)
	{
		qDebug() << "[Satellites] File format is wrong! Error:" << e.what();
		return false;
	}

	return true;
}

bool Satellites::isValidRangeDates(const StelCore *core) const
{
	bool ok;
	double tJD = core->getJD();
	double uJD = StelUtils::getJulianDayFromISO8601String(lastUpdate.toString(Qt::ISODate), &ok);
	if (lastUpdate.isNull()) // No updates yet?
		uJD = tJD;
	// do not draw anything before Oct 4, 1957, 19:28:34GMT ;-)
	// upper limit for drawing is +5 years after latest update of TLE
	if ((tJD<2436116.3115) || (tJD>(uJD+1825)))
		return false;
	else
		return true;
}

#ifdef _OLD_IRIDIUM_PREDICTIONS
IridiumFlaresPredictionList Satellites::getIridiumFlaresPrediction()
{
	StelCore* pcore = StelApp::getInstance().getCore();
	double currentJD = pcore->getJD(); // save current JD
	bool isTimeNow = pcore->getIsTimeNow();
	long iJD =currentJD;
	double JDMidnight = iJD + 0.5 - pcore->getCurrentLocation().longitude / 360.f;
	double delta = currentJD - JDMidnight;

	pcore->setJD(iJD + 0.5 - pcore->getCurrentLocation().longitude / 360.f);
	pcore->update(10); // force update to get new coordinates

	IridiumFlaresPredictionList predictions;
	predictions.clear();
	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->getEnglishName().startsWith("IRIDIUM"))
		{
			double dt  = 0;
			double angle0 = 100;
			double lat0 = -1;
			while (dt<1)
			{
				Satellite::timeShift = dt+delta;
				sat->update(0);

				Vec3d pos = sat->getAltAzPosApparent(pcore);
				double lat = pos.latitude();
				double lon = M_PI - pos.longitude();
				double v =   sat->getVMagnitude(pcore);
				double angle =  sat->sunReflAngle;

				if (angle>0 && angle<2)
				{
					if (angle>angle0 && (v<1) && lat>5*M_PI/180)
					{
						IridiumFlaresPrediction flare;
						flare.datetime = StelUtils::julianDayToISO8601String(currentJD+dt+StelApp::getInstance().getCore()->getUTCOffset(currentJD+dt)/24.f);
						flare.satellite = sat->getEnglishName();
						flare.azimuth   = lon;
						flare.altitude  = lat;
						flare.magnitude = v;

						predictions.append(flare);

						dt +=0.005;
					}
					angle0 = angle;
				}
				if (angle>0)
					dt+=0.000002*angle*angle;
				else
				{
					if (lat0>0 && lat<0)
						dt+=0.05;
					else
						dt+=0.0002;
				}
				lat0 = lat;
			}
		}
	}
	Satellite::timeShift = 0.;
	if (isTimeNow)
		pcore->setTimeNow();
	else
		pcore->setJD(currentJD);

	return predictions;
}
#else

struct SatDataStruct {
	double nextJD;
	double angleToSun;
	double altitude;
	double azimuth;
	double v;
};

IridiumFlaresPredictionList Satellites::getIridiumFlaresPrediction()
{
	StelCore* pcore = StelApp::getInstance().getCore();
	SolarSystem* ssystem = (SolarSystem*)StelApp::getInstance().getModuleMgr().getModule("SolarSystem");

	double currentJD = pcore->getJD(); // save current JD
	bool isTimeNow = pcore->getIsTimeNow();

	double predictionJD = currentJD - 1.;  //  investigate what's seen recently// yesterday
	double predictionEndJD = currentJD + getIridiumFlaresPredictionDepth(); // 7 days interval by default
	pcore->setJD(predictionJD);

	bool useSouthAzimuth = StelApp::getInstance().getFlagSouthAzimuthUsage();

	IridiumFlaresPredictionList predictions;

// create a lіst of Iridiums
	QMap<SatelliteP,SatDataStruct> iridiums;
	SatDataStruct sds;
	double nextJD = predictionJD + 1./24;

	for (const auto& sat : satellites)
	{
		if (sat->initialized && sat->getEnglishName().startsWith("IRIDIUM"))
		{
			Vec3d pos = sat->getAltAzPosApparent(pcore);
			sds.angleToSun = sat->sunReflAngle;
			sds.altitude = pos.latitude();
			sds.azimuth = pos.longitude();
			sds.v = sat->getVMagnitude(pcore);
			double t;
			if (sds.altitude<0)
				t = qMax(-sds.altitude, 1.) / 2880;
			else
			if (sds.angleToSun>0)
				t = qMax(sds.angleToSun, 1.) / (2*86400);
			else
			{
				//qDebug() << "IRIDIUM warn: alt, angleToSun = " <<sds.altitude << sds.angleToSun;
				t = 0.25/1440; // we should never be here, but assuming 1/4 minute to leave this
			}

			sds.nextJD = predictionJD + t;
			iridiums.insert(sat, sds);
			//qDebug() << sat->getEnglishName() << ": "
			//		 << sds.altitude
			//		 << sds.azimuth
			//		 << sds.angleToSun
			//		 << sds.v
			//		 << StelUtils::julianDayToISO8601String(predictionJD+StelApp::getInstance().getCore()->getUTCOffset(predictionJD)/24.f)
			//			;
			if (nextJD>sds.nextJD)
				nextJD = sds.nextJD;
		}
	}
	predictionJD = nextJD;

	while (predictionJD<predictionEndJD)
	{
		nextJD = predictionJD + 1./24;
		pcore->setJD(predictionJD);

		ssystem->getEarth()->computePosition(predictionJD);
		pcore->update(0);

		for (auto i = iridiums.begin(); i != iridiums.end(); ++i)
		{
			if ( i.value().nextJD<=predictionJD)
			{
				i.key()->update(0);

				double v = i.key()->getVMagnitude(pcore);
				bool flareFound = false;
				if (v > i.value().v)
				{
					if (i.value().v < 1. // brighness limit
					 && i.value().angleToSun>0.
					 && i.value().angleToSun<2.)
					{
						IridiumFlaresPrediction flare;
						flare.datetime = StelUtils::julianDayToISO8601String(predictionJD+StelApp::getInstance().getCore()->getUTCOffset(predictionJD)/24.f);
						flare.satellite = i.key()->getEnglishName();
						flare.azimuth   = i.value().azimuth;
						if (useSouthAzimuth)
						{
							flare.azimuth += M_PI;
							if (flare.azimuth > M_PI*2)
								flare.azimuth -= M_PI*2;
						}
						flare.altitude  = i.value().altitude;
						flare.magnitude = i.value().v;

						predictions.append(flare);
						flareFound = true;
						//qDebug() << "Flare:" << flare.datetime << flare.satellite;
					}
				}

/*				qDebug() << QString::asprintf("%20s  alt:%6.1f  az:%6.1f  sun:%6.1f  v:%6.1f",
											 i.key()->getEnglishName().toStdString(),
											 i.value().altitude*180/M_PI,
											 i.value().azimuth*180/M_PI,
											 i.value().angleToSun,
											 v
											 )
						 <<  StelUtils::julianDayToISO8601String(predictionJD+StelApp::getInstance().getCore()->getUTCOffset(predictionJD)/24.f)
							 ;
*/
				Vec3d pos = i.key()->getAltAzPosApparent(pcore);

				i.value().v = flareFound ?  17 : v; // block extra report
				i.value().altitude = pos.latitude();
				i.value().azimuth = M_PI - pos.longitude();
				i.value().angleToSun = i.key()->sunReflAngle;

				double t;
				if (flareFound)
					t = 1./24;
				else
				if (i.value().altitude<0)
					t = qMax((-i.value().altitude)*57,1.) / 5600;
				else
				if (i.value().angleToSun>0)
					t = qMax(i.value().angleToSun,1.) / (4*86400);
				else
				{
					//qDebug() << "IRIDIUM warn2: alt, angleToSun = " <<i.value().altitude << i.value().angleToSun;
					t = 0.25/1440; // we should never be here, but assuming 1/4 minute to leave this
				}
				i.value().nextJD = predictionJD + t;
				if (nextJD>i.value().nextJD)
					nextJD = i.value().nextJD;
			}
		}
		predictionJD = nextJD;
	}

	//Satellite::timeShift = 0.;
	if (isTimeNow)
		pcore->setTimeNow();
	else
		pcore->setJD(currentJD);

	return predictions;
}
#endif

void Satellites::translations()
{
#if 0
	// Satellite groups
	// TRANSLATORS: Satellite group: Bright/naked-eye-visible satellites
	N_("visual");
	// TRANSLATORS: Satellite group: Scientific satellites
	N_("scientific");
	// TRANSLATORS: Satellite group: Communication satellites
	N_("communications");
	// TRANSLATORS: Satellite group: Navigation satellites
	N_("navigation");
	// TRANSLATORS: Satellite group: Amateur radio (ham) satellites
	N_("amateur");
	// TRANSLATORS: Satellite group: Weather (meteorological) satellites
	N_("weather");
	// TRANSLATORS: Satellite group: Satellites in geostationary orbit
	N_("geostationary");
	// TRANSLATORS: Satellite group: Satellites that are no longer functioning
	N_("non-operational");
	// TRANSLATORS: Satellite group: Satellites belonging to the GPS constellation (the Global Positioning System)
	N_("gps");
	// TRANSLATORS: Satellite group: Satellites belonging to the GLONASS constellation (GLObal NAvigation Satellite System)
	N_("glonass");
	// TRANSLATORS: Satellite group: Satellites belonging to the Galileo constellation (global navigation satellite system by the European Union)
	N_("galileo");
	// TRANSLATORS: Satellite group: Satellites belonging to the Iridium constellation (Iridium is a proper name)
	N_("iridium");
	// TRANSLATORS: Satellite group: Space stations
	N_("stations");
	// TRANSLATORS: Satellite group: Education satellites
	N_("education");
	// TRANSLATORS: Satellite group: Satellites belonging to the space observatories
	N_("observatory");
	
	// Satellite descriptions - bright and/or famous objects
	// Just A FEW objects please! (I'm looking at you, Alex!)
	// TRANSLATORS: Satellite description. "Hubble" is a person's name.
	N_("The Hubble Space Telescope");
	// TRANSLATORS: Satellite description.
	N_("The International Space Station");
	// TRANSLATORS: Satellite description.
	N_("China's first space station");
	// TRANSLATORS: Satellite description.
	N_("The russian space radio telescope RadioAstron");
	// TRANSLATORS: Satellite description.
	N_("International Gamma-Ray Astrophysics Laboratory");
	// TRANSLATORS: Satellite description.
	N_("The Gamma-Ray Observatory");
	// TRANSLATORS: Satellite description.
	N_("The Microvariability and Oscillations of Stars telescope");
	// TRANSLATORS: Satellite description.
	N_("The Interface Region Imaging Spectrograph");
	// TRANSLATORS: Satellite description.
	N_("The Spectroscopic Planet Observatory for Recognition of Interaction of Atmosphere");
	// TRANSLATORS: Satellite description.
	M_("Nuclear Spectroscopic Telescope Array");
	// TRANSLATORS: Satellite description.
	N_("The Dark Matter Particle Explorer");
	// TRANSLATORS: Satellite description.
	N_("Arcsecond Space Telescope Enabling Research in Astrophysics");
	// TRANSLATORS: Satellite description.
	N_("Reuven Ramaty High Energy Solar Spectroscopic Imager");
	// TRANSLATORS: Satellite description.
	N_("The Chandra X-ray Observatory");

	// Satellite names - a few famous objects only
	// TRANSLATORS: Satellite name: International Space Station
	N_("ISS (ZARYA)");
	// TRANSLATORS: Satellite name: International Space Station
	N_("ISS");
	// TRANSLATORS: Satellite name: Hubble Space Telescope
	N_("HST");
	// TRANSLATORS: Satellite name: Spektr-R Space Observatory (or RadioAstron)
	N_("SPEKTR-R");
	// TRANSLATORS: Satellite name: International Gamma-Ray Astrophysics Laboratory (INTEGRAL)
	N_("INTEGRAL");
	// TRANSLATORS: China's first space station name
	N_("TIANGONG 1");

#endif
}
