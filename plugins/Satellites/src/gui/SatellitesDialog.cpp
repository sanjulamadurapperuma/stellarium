/*
 * Stellarium Satellites plugin config dialog
 *
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

#include <QDebug>
#include <QDateTime>
#include <QFileDialog>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTimer>
#include <QUrl>
#include <QTabWidget>
#include <QAction>
#include <QColorDialog>

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "ui_satellitesDialog.h"
#include "SatellitesDialog.hpp"
#include "SatellitesImportDialog.hpp"
#include "SatellitesListModel.hpp"
#include "SatellitesListFilterModel.hpp"
#include "Satellites.hpp"
#include "StelModuleMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelMovementMgr.hpp"
#include "StelStyle.hpp"
#include "StelGui.hpp"
#include "StelMainView.hpp"
#include "StelFileMgr.hpp"
#include "StelTranslator.hpp"
#include "StelActionMgr.hpp"
#include "StelUtils.hpp"

#include "external/qxlsx/xlsxdocument.h"
#include "external/qxlsx/xlsxchartsheet.h"
#include "external/qxlsx/xlsxcellrange.h"
#include "external/qxlsx/xlsxchart.h"
#include "external/qxlsx/xlsxrichstring.h"
#include "external/qxlsx/xlsxworkbook.h"
using namespace QXlsx;

SatellitesDialog::SatellitesDialog()
	: StelDialog("Satellites")
	, satelliteModified(false)
	, updateTimer(Q_NULLPTR)
	, importWindow(Q_NULLPTR)
	, filterModel(Q_NULLPTR)
	, checkStateRole(Qt::UserRole)
	, delimiter(", ")
	, acEndl("\n")
{
	ui = new Ui_satellitesDialog;
	iridiumFlaresHeader.clear();
}

SatellitesDialog::~SatellitesDialog()
{
	if (updateTimer)
	{
		updateTimer->stop();
		delete updateTimer;
		updateTimer = Q_NULLPTR;
	}

	if (importWindow)
	{
		delete importWindow;
		importWindow = Q_NULLPTR;
	}

	delete ui;
}

void SatellitesDialog::retranslate()
{
	if (dialog)
	{
		ui->retranslateUi(dialog);
		updateSettingsPage(); // For the button; also calls updateCountdown()
		populateAboutPage();
		populateFilterMenu();
		initListIridiumFlares();
	}
}

// Initialize the dialog widgets and connect the signals/slots
void SatellitesDialog::createDialogContent()
{
	ui->setupUi(dialog);
	ui->tabs->setCurrentIndex(0);
	ui->labelAutoAdd->setVisible(false);
	connect(ui->closeStelWindow, SIGNAL(clicked()), this, SLOT(close()));
	connect(ui->TitleBar, SIGNAL(movedTo(QPoint)), this, SLOT(handleMovedTo(QPoint)));
	connect(&StelApp::getInstance(), SIGNAL(languageChanged()), this, SLOT(retranslate()));
	Satellites* plugin = GETSTELMODULE(Satellites);

	// Kinetic scrolling
	kineticScrollingList << ui->satellitesList << ui->sourceList << ui->aboutTextBrowser;
	StelGui* gui= dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
	if (gui)
	{
		enableKineticScrolling(gui->getFlagUseKineticScrolling());
		connect(gui, SIGNAL(flagUseKineticScrollingChanged(bool)), this, SLOT(enableKineticScrolling(bool)));
	}

#ifdef Q_OS_WIN
	acEndl="\r\n";
#else
	acEndl="\n";
#endif

	// Set symbols on buttons
	ui->addSatellitesButton->setText(QChar(0x2795)); // Heavy plus symbol
	ui->removeSatellitesButton->setText(QChar(0x2796)); // Heavy minus symbol
	ui->satColorPickerButton->setText(QChar(0x2740)); // Florette symbol
	ui->saveSatellitesButton->setText(QString());
	ui->addSourceButton->setText(QChar(0x2795)); // Heavy plus symbol
	ui->deleteSourceButton->setText(QChar(0x2796)); // Heavy minus symbol

	// Settings tab / updates group
	// These controls are refreshed by updateSettingsPage(), which in
	// turn is triggered by setting any of these values. Because
	// clicked() is issued only by user input, there's no endless loop.
	connect(ui->internetUpdatesCheckbox, SIGNAL(clicked(bool)),     plugin, SLOT(enableInternetUpdates(bool)));
	connect(ui->checkBoxAutoAdd,         SIGNAL(clicked(bool)),     plugin, SLOT(enableAutoAdd(bool)));
	connect(ui->checkBoxAutoRemove,      SIGNAL(clicked(bool)),     plugin, SLOT(enableAutoRemove(bool)));
	connect(ui->updateFrequencySpinBox,  SIGNAL(valueChanged(int)), plugin, SLOT(setUpdateFrequencyHours(int)));
	connect(ui->updateButton,            SIGNAL(clicked()),         this,   SLOT(updateTLEs()));
	connect(ui->jumpToSourcesButton,     SIGNAL(clicked()),         this,   SLOT(jumpToSourcesTab()));
	connect(plugin, SIGNAL(updateStateChanged(Satellites::UpdateState)), this, SLOT(showUpdateState(Satellites::UpdateState)));
	connect(plugin, SIGNAL(tleUpdateComplete(int, int, int, int)),       this, SLOT(showUpdateCompleted(int, int, int, int)));

	updateTimer = new QTimer(this);
	connect(updateTimer, SIGNAL(timeout()), this, SLOT(updateCountdown()));
	updateTimer->start(7000);

	// Settings tab / General settings group
	// This does call Satellites::setFlagLabels() indirectly.
	StelAction* action = StelApp::getInstance().getStelActionManager()->findAction("actionShow_Satellite_Labels");
	connect(ui->labelsGroup,           SIGNAL(clicked(bool)),     action, SLOT(setChecked(bool)));
	connect(ui->fontSizeSpinBox,       SIGNAL(valueChanged(int)), plugin, SLOT(setLabelFontSize(int)));
	connect(ui->restoreDefaultsButton, SIGNAL(clicked()),         this,   SLOT(restoreDefaults()));
	connect(ui->saveSettingsButton,    SIGNAL(clicked()),         this,   SLOT(saveSettings()));

	// Settings tab / realistic mode group
	connect(ui->realisticGroup,          SIGNAL(clicked(bool)), this,   SLOT(setFlagRealisticMode(bool)));
	connect(ui->hideInvisibleSatellites, SIGNAL(clicked(bool)), plugin, SLOT(setFlagHideInvisibleSatellites(bool)));

	// Settings tab - populate all values
	updateSettingsPage();

	// Settings tab / orbit lines group
	connect(ui->orbitLinesGroup,   SIGNAL(clicked(bool)),   plugin, SLOT(setFlagOrbitLines(bool)));
	connect(ui->orbitSegmentsSpin, SIGNAL(valueChanged(int)), this, SLOT(setOrbitParams()));
	connect(ui->orbitFadeSpin,     SIGNAL(valueChanged(int)), this, SLOT(setOrbitParams()));
	connect(ui->orbitDurationSpin, SIGNAL(valueChanged(int)), this, SLOT(setOrbitParams()));

	// Satellites tab
	filterModel = new SatellitesListFilterModel(this);
	filterModel->setSourceModel(plugin->getSatellitesListModel());
	filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
	ui->satellitesList->setModel(filterModel);
	connect(ui->lineEditSearch, SIGNAL(textChanged(QString)), filterModel, SLOT(setFilterWildcard(QString)));

	QAction *clearAction = ui->lineEditSearch->addAction(QIcon(":/graphicGui/backspace-white.png"),
							     QLineEdit::ActionPosition::TrailingPosition);
	connect(clearAction, SIGNAL(triggered()), this, SLOT(searchSatellitesClear()));

	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	connect(selectionModel, SIGNAL(selectionChanged(QItemSelection,QItemSelection)),
		     this, SLOT(updateSatelliteData()));
	connect(ui->satellitesList, SIGNAL(doubleClicked(QModelIndex)),
		     this, SLOT(trackSatellite(QModelIndex)));

	// Two-state input, three-state display
	connect(ui->displayedCheckbox, SIGNAL(clicked(bool)), ui->displayedCheckbox, SLOT(setChecked(bool)));
	connect(ui->orbitCheckbox,     SIGNAL(clicked(bool)), ui->orbitCheckbox,     SLOT(setChecked(bool)));
	connect(ui->userCheckBox,      SIGNAL(clicked(bool)), ui->userCheckBox,      SLOT(setChecked(bool)));

	// Because the previous signals and slots were connected first,
	// they will be executed before these.
	connect(ui->displayedCheckbox, SIGNAL(clicked()), this, SLOT(setFlags()));
	connect(ui->orbitCheckbox,     SIGNAL(clicked()), this, SLOT(setFlags()));
	connect(ui->userCheckBox,      SIGNAL(clicked()), this, SLOT(setFlags()));

	connect(ui->satColorPickerButton, SIGNAL(clicked(bool)), this, SLOT(askSatColor()));
	connect(ui->descriptionTextEdit,  SIGNAL(textChanged()), this, SLOT(descriptionTextChanged()));


	connect(ui->groupsListWidget, SIGNAL(itemChanged(QListWidgetItem*)),
		     this, SLOT(handleGroupChanges(QListWidgetItem*)));

	connect(ui->groupFilterCombo,       SIGNAL(currentIndexChanged(int)), this, SLOT(filterListByGroup(int)));
	connect(ui->saveSatellitesButton,   SIGNAL(clicked()),                this, SLOT(saveSatellites()));
	connect(ui->removeSatellitesButton, SIGNAL(clicked()),                this, SLOT(removeSatellites()));

	importWindow = new SatellitesImportDialog();
	connect(ui->addSatellitesButton, SIGNAL(clicked()), importWindow, SLOT(setVisible()));
	connect(importWindow, SIGNAL(satellitesAccepted(TleDataList)), this, SLOT(addSatellites(TleDataList)));

	// Sources tab
	connect(ui->sourceList, SIGNAL(currentTextChanged(const QString&)), ui->sourceEdit, SLOT(setText(const QString&)));
	connect(ui->sourceList, SIGNAL(itemChanged(QListWidgetItem*)),      this,           SLOT(saveSourceList()));
	connect(ui->sourceEdit, SIGNAL(editingFinished()),                  this,           SLOT(saveEditedSource()));
	connect(ui->deleteSourceButton, SIGNAL(clicked()),         this, SLOT(deleteSourceRow()));
	connect(ui->addSourceButton,    SIGNAL(clicked()),         this, SLOT(addSourceRow()));
	connect(plugin,                 SIGNAL(settingsChanged()), this, SLOT(toggleCheckableSources()));

	// bug #1350669 (https://bugs.launchpad.net/stellarium/+bug/1350669)
	connect(ui->sourceList, SIGNAL(currentRowChanged(int)), ui->sourceList, SLOT(repaint()));

	// About tab
	populateAboutPage();

	populateFilterMenu();
	populateSourcesList();

	initListIridiumFlares();
	ui->flaresPredictionDepthSpinBox->setValue(plugin->getIridiumFlaresPredictionDepth());
	connect(ui->flaresPredictionDepthSpinBox, SIGNAL(valueChanged(int)), plugin, SLOT(setIridiumFlaresPredictionDepth(int)));
	connect(ui->predictIridiumFlaresPushButton, SIGNAL(clicked()), this, SLOT(predictIridiumFlares()));
	connect(ui->predictedIridiumFlaresSaveButton, SIGNAL(clicked()), this, SLOT(savePredictedIridiumFlares()));
	connect(ui->iridiumFlaresTreeWidget, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(selectCurrentIridiumFlare(QModelIndex)));
}

// for now, the color picker changes hintColor AND orbitColor at once
void SatellitesDialog::askSatColor()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	QColor c = QColorDialog::getColor(buttonColor, Q_NULLPTR, "");
	if (c.isValid())
	{
		Vec3f vColor = Vec3f(c.redF(), c.greenF(), c.blueF());
		SatelliteP sat;

		// colorize all selected satellites
		for (int i = 0; i < selection.size(); i++)
		{
			const QModelIndex& index = selection.at(i);
			sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());

			sat->hintColor = vColor;
			sat->orbitColor = vColor;
		}

		// colorize the button
		buttonColor = c;
		ui->satColorPickerButton->setStyleSheet("QPushButton { background-color:" + buttonColor.name() + "; }");
	}
}

// save new description text to selected satellite(s)
void SatellitesDialog::descriptionTextChanged()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	QString newdesc = ui->descriptionTextEdit->toPlainText();
	SatelliteP sat;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	for (int i = 0; i < selection.size(); i++)
	{
		const QModelIndex& index = selection.at(i);
		sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());

		if (sat->description != newdesc)
		{
			sat->description = newdesc;
		}
	}
}


void SatellitesDialog::setFlagRealisticMode(bool state)
{
	GETSTELMODULE(Satellites)->setFlagRelisticMode(!state);
}

void SatellitesDialog::searchSatellitesClear()
{
	ui->lineEditSearch->clear();
}

void SatellitesDialog::savePredictedIridiumFlares()
{
	QString filter = q_("Microsoft Excel Open XML Spreadsheet");
	filter.append(" (*.xlsx);;");
	filter.append(q_("CSV (Comma delimited)"));
	filter.append(" (*.csv)");
	QString defaultFilter("(*.xlsx)");
	QString filePath = QFileDialog::getSaveFileName(Q_NULLPTR,
							q_("Save predicted Iridium flares as..."),
							QDir::homePath() + "/iridium_flares.xlsx",
							filter,
							&defaultFilter);

	int count = ui->iridiumFlaresTreeWidget->topLevelItemCount();
	int columns = iridiumFlaresHeader.size();

	if (defaultFilter.contains(".csv", Qt::CaseInsensitive))
	{
		QFile predictedIridiumFlares(filePath);
		if (!predictedIridiumFlares.open(QFile::WriteOnly | QFile::Truncate))
		{
			qWarning() << "[Satellites]: Unable to open file"
					  << QDir::toNativeSeparators(filePath);
			return;
		}

		QTextStream predictedIridiumFlaresList(&predictedIridiumFlares);
		predictedIridiumFlaresList.setCodec("UTF-8");

		predictedIridiumFlaresList << iridiumFlaresHeader.join(delimiter) << acEndl;

		for (int i = 0; i < count; i++)
		{
			int columns = iridiumFlaresHeader.size();
			for (int j=0; j<columns; j++)
			{
				predictedIridiumFlaresList << ui->iridiumFlaresTreeWidget->topLevelItem(i)->text(j);
				if (j<columns-1)
					predictedIridiumFlaresList << delimiter;
				else
					predictedIridiumFlaresList << acEndl;
			}
		}
		predictedIridiumFlares.close();
	}
	else
	{
		int *width;
		width = new int[columns];
		QString sData;
		int w;

		QXlsx::Document xlsx;
		xlsx.setDocumentProperty("title", q_("Predicted Iridium flares"));
		xlsx.setDocumentProperty("creator", StelUtils::getApplicationName());
		xlsx.addSheet(q_("Predicted Iridium flares"), AbstractSheet::ST_WorkSheet);

		QXlsx::Format header;
		header.setHorizontalAlignment(QXlsx::Format::AlignHCenter);
		header.setPatternBackgroundColor(Qt::yellow);
		header.setBorderStyle(QXlsx::Format::BorderThin);
		header.setBorderColor(Qt::black);
		header.setFontBold(true);
		for (int i = 0; i < columns; i++)
		{
			// Row 1: Names of columns
			sData = iridiumFlaresHeader.at(i).trimmed();
			xlsx.write(1, i + 1, sData, header);
			width[i] = sData.size();
		}

		QXlsx::Format data;
		data.setHorizontalAlignment(QXlsx::Format::AlignRight);
		for (int i = 0; i < count; i++)
		{
			for (int j = 0; j < columns; j++)
			{
				// Row 2 and next: the data
				sData = ui->iridiumFlaresTreeWidget->topLevelItem(i)->text(j).trimmed();
				xlsx.write(i + 2, j + 1, sData, data);
				w = sData.size();
				if (w > width[j])
				{
					width[j] = w;
				}
			}
		}

		for (int i = 0; i < columns; i++)
		{
			xlsx.setColumnWidth(i+1, width[i]+2);
		}

		delete[] width;
		xlsx.saveAs(filePath);
	}
}

void SatellitesDialog::filterListByGroup(int index)
{
	if (index < 0)
		return;

	QString groupId = ui->groupFilterCombo->itemData(index).toString();
	if (groupId == "all")
		filterModel->setSecondaryFilters(QString(), SatNoFlags);
	else if (groupId == "[displayed]")
		filterModel->setSecondaryFilters(QString(), SatDisplayed);
	else if (groupId == "[undisplayed]")
		filterModel->setSecondaryFilters(QString(), SatNotDisplayed);
	else if (groupId == "[newlyadded]")
		filterModel->setSecondaryFilters(QString(), SatNew);
	else if (groupId == "[orbiterror]")
		filterModel->setSecondaryFilters(QString(), SatError);
	else
	{
		filterModel->setSecondaryFilters(groupId, SatNoFlags);
	}

	if (ui->satellitesList->model()->rowCount() <= 0)
		return;

	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	QModelIndex first;
	if (selectionModel->hasSelection())
	{
		first = selectionModel->selectedRows().first();
	}
	else
	{
		// Scroll to the top
		first = ui->satellitesList->model()->index(0, 0);
	}
	selectionModel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
	ui->satellitesList->scrollTo(first);
}

void SatellitesDialog::updateSatelliteData()
{
	// NOTE: This was probably going to be used for editing satellites?
	satelliteModified = false;

	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();
	if (selection.isEmpty())
		return; // TODO: Clear the fields?

	enableSatelliteDataForm(false);

	// needed for colorbutton
	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);
	Vec3f vColor;

	// set default
	buttonColor = QColor(QColor::fromRgbF(0.4, 0.4, 0.4));


	if (selection.count() > 1)
	{
		ui->nameEdit->clear();
		ui->noradNumberEdit->clear();
		ui->tleFirstLineEdit->clear();
		ui->tleSecondLineEdit->clear();

		// get color of first selected item and test against all other selections
		{
			const QModelIndex& index = selection.at(0);
			QString id = index.data(Qt::UserRole).toString();
			SatelliteP sat = SatellitesMgr->getById(id);

			vColor = sat->hintColor;

			for (int i = 1; i < selection.size(); i++)
			{
				const QModelIndex& index = selection.at(i);

				id = index.data(Qt::UserRole).toString();
				sat = SatellitesMgr->getById(id);

				// test for more than one color in the selection.
				// if there are, return grey
				if (sat->hintColor != vColor)
				{
					vColor = Vec3f(0.4, 0.4, 0.4);
					break;
				}
			}
		}

		// get description text of first selection and test against all other selections
		{
			const QModelIndex& index = selection.at(0);
			QString descText = index.data(SatDescriptionRole).toString();

			if (!descText.isEmpty())
			{
				for (int i = 1; i < selection.size(); i++)
				{
					const QModelIndex& index = selection.at(i);

					if (descText != index.data(SatDescriptionRole).toString())
					{
						descText.clear();
						break;
					}
				}
			}

			ui->descriptionTextEdit->setText(descText);
		}
	}
	else
	{
		QModelIndex& index = selection.first();
		ui->nameEdit->setText(index.data(Qt::DisplayRole).toString());
		ui->noradNumberEdit->setText(index.data(Qt::UserRole).toString());
		// NOTE: Description is deliberately displayed untranslated!
		ui->descriptionTextEdit->setText(index.data(SatDescriptionRole).toString());
		ui->tleFirstLineEdit->setText(index.data(FirstLineRole).toString());
		ui->tleFirstLineEdit->setCursorPosition(0);
		ui->tleSecondLineEdit->setText(index.data(SecondLineRole).toString());
		ui->tleSecondLineEdit->setCursorPosition(0);
		
		
		// get color of the one selected sat
		QString id = index.data(Qt::UserRole).toString();
		SatelliteP sat = SatellitesMgr->getById(id);
		vColor = sat->hintColor;
	}

	// colorize the colorpicker button
	buttonColor.setRgbF(vColor.v[0], vColor.v[1], vColor.v[2]);
	ui->satColorPickerButton->setStyleSheet("QPushButton { background-color:" + buttonColor.name() + "; }");

	// bug #1350669 (https://bugs.launchpad.net/stellarium/+bug/1350669)
	ui->satellitesList->repaint();

	// Things that are cumulative in a multi-selection
	GroupSet globalGroups = GETSTELMODULE(Satellites)->getGroups();
	GroupSet groupsUsedBySome;
	GroupSet groupsUsedByAll = globalGroups;
	ui->displayedCheckbox->setChecked(false);
	ui->orbitCheckbox->setChecked(false);
	ui->userCheckBox->setChecked(false);

	for (int i = 0; i < selection.size(); i++)
	{
		const QModelIndex& index = selection.at(i);

		// "Displayed" checkbox
		SatFlags flags = index.data(SatFlagsRole).value<SatFlags>();
		if (flags.testFlag(SatDisplayed))
		{
			if (!ui->displayedCheckbox->isChecked())
			{
				if (i == 0)
					ui->displayedCheckbox->setChecked(true);
				else
					ui->displayedCheckbox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->displayedCheckbox->isChecked())
				ui->displayedCheckbox->setCheckState(Qt::PartiallyChecked);

		// "Orbit" box
		if (flags.testFlag(SatOrbit))
		{
			if (!ui->orbitCheckbox->isChecked())
			{
				if (i == 0)
					ui->orbitCheckbox->setChecked(true);
				else
					ui->orbitCheckbox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->orbitCheckbox->isChecked())
				ui->orbitCheckbox->setCheckState(Qt::PartiallyChecked);

		// User ("do not update") box
		if (flags.testFlag(SatUser))
		{
			if (!ui->userCheckBox->isChecked())
			{
				if (i == 0)
					ui->userCheckBox->setChecked(true);
				else
					ui->userCheckBox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->userCheckBox->isChecked())
				ui->userCheckBox->setCheckState(Qt::PartiallyChecked);


		// Accumulating groups
		GroupSet groups = index.data(SatGroupsRole).value<GroupSet>();
		groupsUsedBySome.unite(groups);
		groupsUsedByAll.intersect(groups);
	}

	// Repopulate the group selector
	// Nice list of checkable, translated groups that allows adding new groups
	ui->groupsListWidget->blockSignals(true);
	ui->groupsListWidget->clear();
	for (const auto& group : globalGroups)
	{
		QListWidgetItem* item = new QListWidgetItem(q_(group),
							    ui->groupsListWidget);
		item->setData(Qt::UserRole, group);
		Qt::CheckState state = Qt::Unchecked;
		if (groupsUsedByAll.contains(group))
			state = Qt::Checked;
		else if (groupsUsedBySome.contains(group))
			state = Qt::PartiallyChecked;
		item->setData(Qt::CheckStateRole, state);
	}
	ui->groupsListWidget->sortItems();
	addSpecialGroupItem(); // Add the "Add new..." line
	ui->groupsListWidget->blockSignals(false);

	enableSatelliteDataForm(true);
}

void SatellitesDialog::saveSatellites(void)
{
	GETSTELMODULE(Satellites)->saveCatalog();
}

void SatellitesDialog::populateAboutPage()
{
	// Regexp to replace {text} with an HTML link.
	QRegExp a_rx = QRegExp("[{]([^{]*)[}]");

	QString jsonFileName("<tt>satellites.json</tt>");
	QString oldJsonFileName("<tt>satellites.json.old</tt>");
	QString html = "<html><head></head><body>";
	html += "<h2>" + q_("Stellarium Satellites Plugin") + "</h2><table width=\"90%\">";
	html += "<tr width=\"30%\"><td><strong>" + q_("Version") + "</strong></td><td>" + SATELLITES_PLUGIN_VERSION + "</td></td>";
	html += "<tr><td><strong>" + q_("License") + ":</strong></td><td>" + SATELLITES_PLUGIN_LICENSE + "</td></tr>";
	html += "<tr><td rowspan=2><strong>" + q_("Authors") + "</strong></td><td>Matthew Gates &lt;matthewg42@gmail.com&gt;</td></td>";
	html += "<tr><td>Jose Luis Canales &lt;jlcanales.gasco@gmail.com&gt;</td></tr>";
	html += "<tr><td rowspan=4><strong>" + q_("Contributors") + "</strong></td><td>Bogdan Marinov &lt;bogdan.marinov84@gmail.com&gt;</td></tr>";
	html += "<tr><td>Nick Fedoseev &lt;nick.ut2uz@gmail.com&gt; (" + q_("Iridium flares") + ")</td></tr>";
	html += "<tr><td>Alexander Wolf &lt;alex.v.wolf@gmail.com&gt;</td></tr>";
	html += "<tr><td>Georg Zotti</td></tr></table>";

	html += "<p>" + q_("The Satellites plugin predicts the positions of artificial satellites in Earth orbit.") + "</p>";

	html += "<h3>" + q_("Notes for users") + "</h3><p><ul>";
	html += "<li>" + q_("Satellites and their orbits are only shown when the observer is on Earth.") + "</li>";
	html += "<li>" + q_("Predicted positions are only good for a fairly short time (on the order of days, weeks or perhaps a month into the past and future). Expect high weirdness when looking at dates outside this range.") + "</li>";
	html += "<li>" + q_("Orbital elements go out of date pretty quickly (over mere weeks, sometimes days).  To get useful data out, you need to update the TLE data regularly.") + "</li>";
	// TRANSLATORS: The translated names of the button and the tab are filled in automatically. You can check the original names in Stellarium. File names are not translated.
	QString resetSettingsText = QString(q_("Clicking the \"%1\" button in the \"%2\" tab of this dialog will revert to the default %3 file.  The old file will be backed up as %4.  This can be found in the user data directory, under \"modules/Satellites/\"."))
			.arg(ui->restoreDefaultsButton->text())
			.arg(ui->tabs->tabText(ui->tabs->indexOf(ui->settingsTab)))
			.arg(jsonFileName)
			.arg(oldJsonFileName);
	html += "<li>" + resetSettingsText + "</li>";
	html += "<li>" + q_("The Satellites plugin is still under development.  Some features are incomplete, missing or buggy.") + "</li>";
	html += "</ul></p>";

	// TRANSLATORS: Title of a section in the About tab of the Satellites window
	html += "<h3>" + q_("TLE data updates") + "</h3>";
	html += "<p>" + q_("The Satellites plugin can automatically download TLE data from Internet sources, and by default the plugin will do this if the existing data is more than 72 hours old. ");
	html += "</p><p>" + QString(q_("If you disable Internet updates, you may update from a file on your computer.  This file must be in the same format as the Celestrak updates (see %1 for an example).").arg("<a href=\"http://celestrak.com/NORAD/elements/visual.txt\">visual.txt</a>"));
	html += "</p><p>" + q_("<b>Note:</b> if the name of a satellite in update data has anything in square brackets at the end, it will be removed before the data is used.");
	html += "</p>";

	html += "<h3>" + q_("Adding new satellites") + "</h3>";
	html += "<ol><li>" + q_("Make sure the satellite(s) you wish to add are included in one of the URLs listed in the Sources tab of the satellites configuration dialog.") + "</li>";
	html += "<li>" + q_("Go to the Satellites tab, and click the '+' button.  Select the satellite(s) you wish to add and select the 'add' button.") + "</li></ol>";

	html += "<h3>" + q_("Technical notes") + "</h3>";
	html += "<p>" + q_("Positions are calculated using the SGP4 & SDP4 methods, using NORAD TLE data as the input. ");
	html += q_("The orbital calculation code is written by Jose Luis Canales according to the revised Spacetrack Report #3 (including Spacetrack Report #6). ");
	// TRANSLATORS: The numbers contain the opening and closing tag of an HTML link
	html += QString(q_("See %1this document%2 for details.")).arg("<a href=\"http://www.celestrak.com/publications/AIAA/2006-6753\">").arg("</a>") + "</p>";

	html += "<h3>" + q_("Links") + "</h3>";
	html += "<p>" + QString(q_("Support is provided via the Github website.  Be sure to put \"%1\" in the subject when posting.")).arg("Satellites plugin") + "</p>";
	html += "<p><ul>";
	// TRANSLATORS: The text between braces is the text of an HTML link.
	html += "<li>" + q_("If you have a question, you can {get an answer here}.").toHtmlEscaped().replace(a_rx, "<a href=\"https://groups.google.com/forum/#!forum/stellarium\">\\1</a>") + "</li>";
	// TRANSLATORS: The text between braces is the text of an HTML link.
	html += "<li>" + q_("Bug reports and feature requests can be made {here}.").toHtmlEscaped().replace(a_rx, "<a href=\"https://github.com/Stellarium/stellarium/issues\">\\1</a>") + "</li>";
	// TRANSLATORS: The text between braces is the text of an HTML link.
	html += "<li>" + q_("If you want to read full information about this plugin and its history, you can {get info here}.").toHtmlEscaped().replace(a_rx, "<a href=\"http://stellarium.sourceforge.net/wiki/index.php/Satellites_plug-in\">\\1</a>") + "</li>";
	html += "</ul></p></body></html>";

	StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
	Q_ASSERT(gui);
	QString htmlStyleSheet(gui->getStelStyle().htmlStyleSheet);
	ui->aboutTextBrowser->document()->setDefaultStyleSheet(htmlStyleSheet);

	ui->aboutTextBrowser->setHtml(html);
}

void SatellitesDialog::jumpToSourcesTab()
{
	ui->tabs->setCurrentWidget(ui->sourcesTab);
}

void SatellitesDialog::updateCountdown()
{
	QString nextUpdate = q_("Next update");
	Satellites* plugin = GETSTELMODULE(Satellites);
	bool updatesEnabled = plugin->getUpdatesEnabled();

	if (!updatesEnabled)
		ui->nextUpdateLabel->setText(q_("Internet updates disabled"));
	else if (plugin->getUpdateState() == Satellites::Updating)
		ui->nextUpdateLabel->setText(q_("Updating now..."));
	else
	{
		int secondsToUpdate = plugin->getSecondsToUpdate();
		if (secondsToUpdate <= 60)
			ui->nextUpdateLabel->setText(QString("%1: %2").arg(nextUpdate, q_("< 1 minute")));
		else if (secondsToUpdate < 3600)
		{
			int n = (secondsToUpdate/60)+1;
			// TRANSLATORS: minutes.
			ui->nextUpdateLabel->setText(QString("%1: %2 %3").arg(nextUpdate, QString::number(n), qc_("m", "time")));
		}
		else
		{
			int n = (secondsToUpdate/3600)+1;
			// TRANSLATORS: hours.
			ui->nextUpdateLabel->setText(QString("%1: %2 %3").arg(nextUpdate, QString::number(n), qc_("h", "time")));
		}
	}
}

void SatellitesDialog::showUpdateState(Satellites::UpdateState state)
{
	if (state==Satellites::Updating)
		ui->nextUpdateLabel->setText(q_("Updating now..."));
	else if (state==Satellites::DownloadError || state==Satellites::OtherError)
	{
		ui->nextUpdateLabel->setText(q_("Update error"));
		updateTimer->start();  // make sure message is displayed for a while...
	}
}

void SatellitesDialog::showUpdateCompleted(int updated,
					   int total,
					   int added,
					   int missing)
{
	Satellites* plugin = GETSTELMODULE(Satellites);
	QString message;
	if (plugin->isAutoRemoveEnabled())
		message = q_("Updated %1/%2 satellite(s); %3 added; %4 removed");
	else
		message = q_("Updated %1/%2 satellite(s); %3 added; %4 missing");
	ui->nextUpdateLabel->setText(message.arg(updated).arg(total).arg(added).arg(missing));
	// display the status for another full interval before refreshing status
	updateTimer->start();
	ui->lastUpdateDateTimeEdit->setDateTime(plugin->getLastUpdate());
	//QTimer *timer = new QTimer(this); // FIXME: What's the point of this? --BM. GZ Indeed, never triggered. Remove?
	//connect(timer, SIGNAL(timeout()), this, SLOT(updateCountdown()));
}

void SatellitesDialog::saveEditedSource()
{
	// don't update the currently selected item in the source list if the text is empty or not a valid URL.
	QString u = ui->sourceEdit->text();
	if (u.isEmpty() || u=="")
	{
		qDebug() << "SatellitesDialog::sourceEditingDone empty string - not saving";
		return;
	}

	if (!QUrl(u).isValid() || !u.contains("://"))
	{
		qDebug() << "SatellitesDialog::sourceEditingDone invalid URL - not saving : " << u;
		return;
	}

	// Changes to item data (text or check state) are connected to
	// saveSourceList(), so there's no need to call it explicitly.
	if (ui->sourceList->currentItem()!=Q_NULLPTR)
		ui->sourceList->currentItem()->setText(u);
	else if (ui->sourceList->findItems(u, Qt::MatchExactly).count() <= 0)
	{
		QListWidgetItem* i = new QListWidgetItem(u, ui->sourceList);
		i->setData(checkStateRole, Qt::Unchecked);
		i->setSelected(true);
	}
}

void SatellitesDialog::saveSourceList(void)
{
	QStringList allSources;
	for(int i=0; i<ui->sourceList->count(); i++)
	{
		QString url = ui->sourceList->item(i)->text();
		if (ui->sourceList->item(i)->data(checkStateRole) == Qt::Checked)
			url.prepend("1,");
		allSources << url;
	}
	GETSTELMODULE(Satellites)->setTleSources(allSources);
}

void SatellitesDialog::deleteSourceRow(void)
{
	ui->sourceEdit->clear();
	if (ui->sourceList->currentItem())
		delete ui->sourceList->currentItem();

	saveSourceList();
}

void SatellitesDialog::addSourceRow(void)
{
	ui->sourceList->setCurrentItem(Q_NULLPTR);
	ui->sourceEdit->setText(q_("[new source]"));
	ui->sourceEdit->selectAll();
	ui->sourceEdit->setFocus();
}

void SatellitesDialog::toggleCheckableSources()
{
	QListWidget* list = ui->sourceList;
	if (list->count() < 1)
		return; // Saves effort checking it on every step

	bool enabled = ui->checkBoxAutoAdd->isChecked(); // proxy :)
	if (!enabled == list->item(0)->data(Qt::CheckStateRole).isNull())
		return; // Nothing to do

	ui->sourceList->blockSignals(true); // Prevents saving the list...
	for (int row = 0; row < list->count(); row++)
	{
		QListWidgetItem* item = list->item(row);
		if (enabled)
		{
			item->setData(Qt::CheckStateRole, item->data(Qt::UserRole));
		}
		else
		{
			item->setData(Qt::UserRole, item->data(Qt::CheckStateRole));
			item->setData(Qt::CheckStateRole, QVariant());
		}
	}
	ui->sourceList->blockSignals(false);

	checkStateRole = enabled ? Qt::CheckStateRole : Qt::UserRole;
}

void SatellitesDialog::restoreDefaults(void)
{
	qDebug() << "Satellites::restoreDefaults";
	GETSTELMODULE(Satellites)->restoreDefaults();
	GETSTELMODULE(Satellites)->loadSettings();
	updateSettingsPage();
	populateFilterMenu();
	populateSourcesList();
}

void SatellitesDialog::updateSettingsPage()
{
	Satellites* plugin = GETSTELMODULE(Satellites);

	// Update stuff
	bool updatesEnabled = plugin->getUpdatesEnabled();
	ui->internetUpdatesCheckbox->setChecked(updatesEnabled);
	if(updatesEnabled)
		ui->updateButton->setText(q_("Update now"));
	else
		ui->updateButton->setText(q_("Update from files"));
	ui->checkBoxAutoAdd->setChecked(plugin->isAutoAddEnabled());
	ui->checkBoxAutoRemove->setChecked(plugin->isAutoRemoveEnabled());
	ui->lastUpdateDateTimeEdit->setDateTime(plugin->getLastUpdate());
	ui->updateFrequencySpinBox->setValue(plugin->getUpdateFrequencyHours());

	updateCountdown();

	// Presentation stuff
	ui->labelsGroup->setChecked(plugin->getFlagLabels());
	ui->fontSizeSpinBox->setValue(plugin->getLabelFontSize());

	ui->orbitLinesGroup->setChecked(plugin->getFlagOrbitLines());
	ui->orbitSegmentsSpin->setValue(Satellite::orbitLineSegments);
	ui->orbitFadeSpin->setValue(Satellite::orbitLineFadeSegments);
	ui->orbitDurationSpin->setValue(Satellite::orbitLineSegmentDuration);

	ui->realisticGroup->setChecked(!plugin->getFlagRealisticMode());
	ui->hideInvisibleSatellites->setChecked(plugin->getFlagHideInvisibleSatellites());
}

void SatellitesDialog::populateFilterMenu()
{
	// Save current selection, if any...
	QString selectedId;
	int index = ui->groupFilterCombo->currentIndex();
	if (ui->groupFilterCombo->count() > 0 && index >= 0)
	{
		selectedId = ui->groupFilterCombo->itemData(index).toString();
	}

	// Prevent the list from re-filtering
	ui->groupFilterCombo->blockSignals(true);

	// Populate with group names/IDs
	ui->groupFilterCombo->clear();
	for (const auto& group : GETSTELMODULE(Satellites)->getGroupIdList())
	{
		ui->groupFilterCombo->addItem(q_(group), group);
	}
	ui->groupFilterCombo->model()->sort(0);

	// Add special groups - their IDs deliberately use JSON-incompatible chars.
	ui->groupFilterCombo->insertItem(0, q_("[orbit calculation error]"), QVariant("[orbiterror]"));
	ui->groupFilterCombo->insertItem(0, q_("[all newly added]"), QVariant("[newlyadded]"));
	ui->groupFilterCombo->insertItem(0, q_("[all not displayed]"), QVariant("[undisplayed]"));
	ui->groupFilterCombo->insertItem(0, q_("[all displayed]"), QVariant("[displayed]"));
	ui->groupFilterCombo->insertItem(0, q_("[all]"), QVariant("all"));

	// Restore current selection
	index = 0;
	if (!selectedId.isEmpty())
	{
		index = ui->groupFilterCombo->findData(selectedId);
		if (index < 0)
			index = 0;
	}
	ui->groupFilterCombo->setCurrentIndex(index);
	ui->groupFilterCombo->blockSignals(false);
}

void SatellitesDialog::populateSourcesList()
{
	ui->sourceList->blockSignals(true);
	ui->sourceList->clear();

	Satellites* plugin = GETSTELMODULE(Satellites);
	QStringList urls = plugin->getTleSources();
	checkStateRole = plugin->isAutoAddEnabled() ? Qt::CheckStateRole
						    : Qt::UserRole;
	for (auto url : urls)
	{
		bool checked = false;
		if (url.startsWith("1,"))
		{
			checked = true;
			url.remove(0, 2);
		}
		else if (url.startsWith("0,"))
			url.remove(0, 2);
		QListWidgetItem* item = new QListWidgetItem(url, ui->sourceList);
		item->setData(checkStateRole, checked ? Qt::Checked : Qt::Unchecked);
	}
	ui->sourceList->blockSignals(false);

	if (ui->sourceList->count() > 0) ui->sourceList->setCurrentRow(0);
}

void SatellitesDialog::addSpecialGroupItem()
{
	// TRANSLATORS: Displayed in the satellite group selection box.
	QListWidgetItem* item = new QListWidgetItem(q_("New group..."));
	item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsEditable|Qt::ItemIsSelectable);
	// Assuming this is also the font used for the list items...
	QFont font = ui->groupsListWidget->font();
	font.setItalic(true);
	item->setFont(font);
	ui->groupsListWidget->insertItem(0, item);
}

void SatellitesDialog::setGroups()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();
	if (selection.isEmpty())
		return;

	// Let's determine what to add or remove
	// (partially checked groups are not modified)
	GroupSet groupsToAdd;
	GroupSet groupsToRemove;
	for (int row = 0; row < ui->groupsListWidget->count(); row++)
	{
		QListWidgetItem* item = ui->groupsListWidget->item(row);
		if (item->flags().testFlag(Qt::ItemIsEditable))
			continue;
		if (item->checkState() == Qt::Checked)
			groupsToAdd.insert(item->data(Qt::UserRole).toString());
		else if (item->checkState() == Qt::Unchecked)
			groupsToRemove.insert(item->data(Qt::UserRole).toString());
	}
	for (int i = 0; i < selection.count(); i++)
	{
		const QModelIndex& index = selection.at(i);
		GroupSet groups = index.data(SatGroupsRole).value<GroupSet>();
		groups.subtract(groupsToRemove);
		groups.unite(groupsToAdd);
		QVariant newGroups = QVariant::fromValue<GroupSet>(groups);
		ui->satellitesList->model()->setData(index, newGroups, SatGroupsRole);
	}
}

void SatellitesDialog::saveSettings(void)
{
	GETSTELMODULE(Satellites)->saveSettings();
	GETSTELMODULE(Satellites)->saveCatalog();
}

void SatellitesDialog::addSatellites(const TleDataList& newSatellites)
{
	GETSTELMODULE(Satellites)->add(newSatellites);
	saveSatellites();

	// Trigger re-loading the list to display the new satellites
	int index = ui->groupFilterCombo->findData(QVariant("[newlyadded]"));
	// TODO: Unnecessary once the model can handle changes? --BM
	if (ui->groupFilterCombo->currentIndex() == index)
		filterListByGroup(index);
	else
		ui->groupFilterCombo->setCurrentIndex(index); //Triggers the same operation

	// Select the satellites that were added just now
	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	selectionModel->clearSelection();
	QModelIndex firstSelectedIndex;
	QSet<QString> newIds;
	for (const auto& sat : newSatellites)
		newIds.insert(sat.id);
	for (int row = 0; row < ui->satellitesList->model()->rowCount(); row++)
	{
		QModelIndex index = ui->satellitesList->model()->index(row, 0);
		QString id = index.data(Qt::UserRole).toString();
		if (newIds.remove(id))
		{
			selectionModel->select(index, QItemSelectionModel::Select);
			if (!firstSelectedIndex.isValid())
				firstSelectedIndex = index;
		}
	}
	if (firstSelectedIndex.isValid())
		ui->satellitesList->scrollTo(firstSelectedIndex, QAbstractItemView::PositionAtTop);
	else
		ui->satellitesList->scrollToTop();
}

void SatellitesDialog::removeSatellites()
{
	QStringList idList;
	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	QModelIndexList selectedIndexes = selectionModel->selectedRows();
	for (const auto& index : selectedIndexes)
	{
		QString id = index.data(Qt::UserRole).toString();
		idList.append(id);
	}
	if (!idList.isEmpty())
	{
		GETSTELMODULE(Satellites)->remove(idList);
		saveSatellites();
	}
}

void SatellitesDialog::setFlags()
{
	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	QModelIndexList selection = selectionModel->selectedIndexes();
	for (int row = 0; row < selection.count(); row++)
	{
		const QModelIndex& index = selection.at(row);
		SatFlags flags = index.data(SatFlagsRole).value<SatFlags>();

		// If a checkbox is partially checked, the respective flag is not
		// changed.
		if (ui->displayedCheckbox->isChecked())
			flags |= SatDisplayed;
		else if (ui->displayedCheckbox->checkState() == Qt::Unchecked)
			flags &= ~SatDisplayed;

		if (ui->orbitCheckbox->isChecked())
			flags |= SatOrbit;
		else if (ui->orbitCheckbox->checkState() == Qt::Unchecked)
			flags &= ~SatOrbit;

		if (ui->userCheckBox->isChecked())
			flags |= SatUser;
		else if (ui->userCheckBox->checkState() == Qt::Unchecked)
			flags &= ~SatUser;

		QVariant value = QVariant::fromValue<SatFlags>(flags);
		ui->satellitesList->model()->setData(index, value, SatFlagsRole);
	}
}

void SatellitesDialog::handleGroupChanges(QListWidgetItem* item)
{
	ui->groupsListWidget->blockSignals(true);
	Qt::ItemFlags flags = item->flags();
	if (flags.testFlag(Qt::ItemIsEditable))
	{
		// Harmonize the item with the rest...
		flags ^= Qt::ItemIsEditable;
		item->setFlags(flags | Qt::ItemIsUserCheckable | Qt::ItemIsTristate);
		item->setCheckState(Qt::Checked);
		QString groupId = item->text().trimmed();
		item->setData(Qt::UserRole, groupId);
		QFont font = item->font();
		font.setItalic(false);
		item->setFont(font);

		// ...and add a new one in its place.
		addSpecialGroupItem();

		GETSTELMODULE(Satellites)->addGroup(groupId);
		populateFilterMenu();
	}
	ui->groupsListWidget->blockSignals(false);
	setGroups();
}

void SatellitesDialog::trackSatellite(const QModelIndex& index)
{
	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);
	QString id = index.data(Qt::UserRole).toString();
	SatelliteP sat = SatellitesMgr->getById(id);
	if (sat.isNull())
		return;

	if (!sat->orbitValid)
		return;

	// Turn on Satellite rendering if it is not already on
	sat->displayed = true;

	// If Satellites are not currently displayed, make them visible.
	if (!SatellitesMgr->getFlagHints())
	{
		StelAction* setHintsAction = StelApp::getInstance().getStelActionManager()->findAction("actionShow_Satellite_Hints");
		Q_ASSERT(setHintsAction);
		setHintsAction->setChecked(true);
	}

	StelObjectP obj = qSharedPointerDynamicCast<StelObject>(sat);
	StelObjectMgr& objectMgr = StelApp::getInstance().getStelObjectMgr();
	if (objectMgr.setSelectedObject(obj))
	{
		GETSTELMODULE(StelMovementMgr)->autoZoomIn();
		GETSTELMODULE(StelMovementMgr)->setFlagTracking(true);
	}
}

void SatellitesDialog::setOrbitParams(void)
{
	Satellite::orbitLineSegments = ui->orbitSegmentsSpin->value();
	Satellite::orbitLineFadeSegments = ui->orbitFadeSpin->value();
	Satellite::orbitLineSegmentDuration = ui->orbitDurationSpin->value();
	GETSTELMODULE(Satellites)->recalculateOrbitLines();
}

void SatellitesDialog::updateTLEs(void)
{
	if(GETSTELMODULE(Satellites)->getUpdatesEnabled())
	{
		GETSTELMODULE(Satellites)->updateFromOnlineSources();
	}
	else
	{
		QStringList updateFiles = QFileDialog::getOpenFileNames(Q_NULLPTR,
									q_("Select TLE Update File"),
									StelFileMgr::getDesktopDir(),
									"*.*");
		GETSTELMODULE(Satellites)->updateFromFiles(updateFiles, false);
	}
}

void SatellitesDialog::enableSatelliteDataForm(bool enabled)
{
	// NOTE: I'm still not sure if this is necessary, if the right signals are used to trigger changes...--BM
	ui->displayedCheckbox->blockSignals(!enabled);
	ui->orbitCheckbox->blockSignals(!enabled);
	ui->userCheckBox->blockSignals(!enabled);
	ui->descriptionTextEdit->blockSignals(!enabled);
}

void SatellitesDialog::setIridiumFlaresHeaderNames()
{
	iridiumFlaresHeader.clear();

	iridiumFlaresHeader << q_("Time");
	iridiumFlaresHeader << q_("Brightness");
	iridiumFlaresHeader << q_("Altitude");
	iridiumFlaresHeader << q_("Azimuth");
	iridiumFlaresHeader << q_("Satellite");

	ui->iridiumFlaresTreeWidget->setHeaderLabels(iridiumFlaresHeader);

	// adjust the column width
	for(int i = 0; i < IridiumFlaresCount; ++i)
	{
	    ui->iridiumFlaresTreeWidget->resizeColumnToContents(i);
	}

	// sort-by-date
	ui->iridiumFlaresTreeWidget->sortItems(IridiumFlaresDate, Qt::AscendingOrder);
}

void SatellitesDialog::initListIridiumFlares()
{
	ui->iridiumFlaresTreeWidget->clear();
	ui->iridiumFlaresTreeWidget->setColumnCount(IridiumFlaresCount);
	setIridiumFlaresHeaderNames();
	ui->iridiumFlaresTreeWidget->header()->setSectionsMovable(false);
}

void SatellitesDialog::predictIridiumFlares()
{
	IridiumFlaresPredictionList predictions = GETSTELMODULE(Satellites)->getIridiumFlaresPrediction();

	ui->iridiumFlaresTreeWidget->clear();
	for (const auto& flare : predictions)
	{
		SatPIFTreeWidgetItem *treeItem = new SatPIFTreeWidgetItem(ui->iridiumFlaresTreeWidget);
		QString dt = flare.datetime;
		treeItem->setText(IridiumFlaresDate, QString("%1 %2").arg(dt.left(10)).arg(dt.right(8)));
		treeItem->setText(IridiumFlaresMagnitude, QString::number(flare.magnitude,'f',1));
		treeItem->setTextAlignment(IridiumFlaresMagnitude, Qt::AlignRight);
		treeItem->setText(IridiumFlaresAltitude, StelUtils::radToDmsStr(flare.altitude));
		treeItem->setTextAlignment(IridiumFlaresAltitude, Qt::AlignRight);
		treeItem->setText(IridiumFlaresAzimuth, StelUtils::radToDmsStr(flare.azimuth));
		treeItem->setTextAlignment(IridiumFlaresAzimuth, Qt::AlignRight);
		treeItem->setText(IridiumFlaresSatellite, flare.satellite);
	}

	for(int i = 0; i < IridiumFlaresCount; ++i)
	{
	    ui->iridiumFlaresTreeWidget->resizeColumnToContents(i);
	}
}

void SatellitesDialog::selectCurrentIridiumFlare(const QModelIndex &modelIndex)
{
	StelCore* core = StelApp::getInstance().getCore();
	// Find the object
	QString name = modelIndex.sibling(modelIndex.row(), IridiumFlaresSatellite).data().toString();
	QString date = modelIndex.sibling(modelIndex.row(), IridiumFlaresDate).data().toString();
	bool ok;
	double JD  = StelUtils::getJulianDayFromISO8601String(date.left(10) + "T" + date.right(8), &ok);
	JD -= core->getUTCOffset(JD)/24.;
	JD -= core->JD_SECOND*15; // Set start point on 15 seconds before flash (TODO: should be an option in the GUI?)

	StelObjectMgr* objectMgr = GETSTELMODULE(StelObjectMgr);
	if (objectMgr->findAndSelectI18n(name) || objectMgr->findAndSelect(name))
	{
		StelApp::getInstance().getCore()->setJD(JD);
		const QList<StelObjectP> newSelected = objectMgr->getSelectedObject();
		if (!newSelected.empty())
		{
			StelMovementMgr* mvmgr = GETSTELMODULE(StelMovementMgr);
			mvmgr->moveToObject(newSelected[0], mvmgr->getAutoMoveDuration());
			mvmgr->setFlagTracking(true);
		}
	}
}
