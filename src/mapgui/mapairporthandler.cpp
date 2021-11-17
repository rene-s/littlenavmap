/*****************************************************************************
* Copyright 2015-2021 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "mapgui/mapairporthandler.h"

#include "settings/settings.h"
#include "common/constants.h"
#include "navapp.h"
#include "atools.h"
#include "common/unit.h"
#include "mapgui/mapwidget.h"
#include "mapgui/maplayer.h"
#include "options/optiondata.h"
#include "ui_mainwindow.h"
#include "gui/signalblocker.h"

#include <QWidgetAction>
#include <QDebug>
#include <QStringBuilder>

static const int MIN_SLIDER_ALL_FT = 0;
static const int MAX_SLIDER_FT = 140;

static const int MIN_SLIDER_ALL_METER = 0;
static const int MAX_SLIDER_METER = 50;

namespace internal {

SliderAction::SliderAction(QObject *parent) : QWidgetAction(parent)
{
  sliderValue = minValue();
  setValue(sliderValue);
}

int SliderAction::getSliderValue() const
{
  // -1 is unlimited
  return sliderValue == minValue() ? -1 : sliderValue;
}

void SliderAction::saveState()
{
  atools::settings::Settings::instance().setValue(lnm::MAP_AIRPORT_RUNWAY_LENGTH, sliderValue);
}

void SliderAction::restoreState()
{
  sliderValue = atools::settings::Settings::instance().valueInt(lnm::MAP_AIRPORT_RUNWAY_LENGTH, minValue());
  setValue(sliderValue);
}

void SliderAction::optionsChanged()
{
  // Set all sliders to new range for given unit and reset to unlimited
  // Block signals to avoid recursion
  sliderValue = minValue();
  for(QSlider *slider : sliders)
  {
    slider->blockSignals(true);
    slider->setValue(sliderValue);
    slider->setMinimum(minValue());
    slider->setMaximum(maxValue());
    slider->blockSignals(false);
  }
}

QWidget *SliderAction::createWidget(QWidget *parent)
{
  QSlider *slider = new QSlider(Qt::Horizontal, parent);
  slider->setMinimum(minValue());
  slider->setMaximum(maxValue());
  slider->setTickPosition(QSlider::TicksBothSides);
  slider->setTickInterval(10);
  slider->setPageStep(10);
  slider->setSingleStep(10);
  slider->setTracking(true);
  slider->setValue(sliderValue);
  slider->setToolTip(tr("Set minimum runway length for airports to display.\n"
                        "Runway length might be also affected by zoom distance."));

  connect(slider, &QSlider::valueChanged, this, &SliderAction::sliderValueChanged);
  connect(slider, &QSlider::valueChanged, this, &SliderAction::valueChanged);
  connect(slider, &QSlider::sliderReleased, this, &SliderAction::sliderReleased);

  // Add to list (register)
  sliders.append(slider);
  return slider;
}

void SliderAction::deleteWidget(QWidget *widget)
{
  QSlider *slider = dynamic_cast<QSlider *>(widget);
  if(slider != nullptr)
  {
    disconnect(slider, &QSlider::valueChanged, this, &SliderAction::sliderValueChanged);
    disconnect(slider, &QSlider::valueChanged, this, &SliderAction::valueChanged);
    disconnect(slider, &QSlider::sliderReleased, this, &SliderAction::sliderReleased);
    sliders.removeAll(slider);
    delete widget;
  }
}

void SliderAction::sliderValueChanged(int value)
{
  sliderValue = value;
  setValue(value);
}

int SliderAction::minValue() const
{
  opts::UnitShortDist unitShortDist = Unit::getUnitShortDist();
  switch(unitShortDist)
  {
    case opts::DIST_SHORT_FT:
      return MIN_SLIDER_ALL_FT;

    case opts::DIST_SHORT_METER:
      return MIN_SLIDER_ALL_METER;
  }
  return MIN_SLIDER_ALL_FT;
}

int SliderAction::maxValue() const
{
  opts::UnitShortDist unitShortDist = Unit::getUnitShortDist();
  switch(unitShortDist)
  {
    case opts::DIST_SHORT_FT:
      return MAX_SLIDER_FT;

    case opts::DIST_SHORT_METER:
      return MAX_SLIDER_METER;
  }
  return MAX_SLIDER_FT;
}

void SliderAction::setValue(int value)
{
  for(QSlider *s : sliders)
  {
    s->blockSignals(true);
    s->setValue(value);
    s->blockSignals(false);
  }
}

void SliderAction::reset()
{
  sliderValue = minValue();
  setValue(sliderValue);
}

// =======================================================================================

/*
 * Wrapper for label action.
 */
class LabelAction
  : public QWidgetAction
{
public:
  LabelAction(QObject *parent) : QWidgetAction(parent)
  {
  }

  void setText(const QString& textParam);

protected:
  /* Create a delete widget for more than one menu (tearout and normal) */
  virtual QWidget *createWidget(QWidget *parent) override;
  virtual void deleteWidget(QWidget *widget) override;

  /* List of created/registered labels */
  QVector<QLabel *> labels;
  QString text;
};

void LabelAction::setText(const QString& textParam)
{
  text = textParam;
  // Set text to all registered labels
  for(QLabel *label : labels)
    label->setText(text);
}

QWidget *LabelAction::createWidget(QWidget *parent)
{
  QLabel *label = new QLabel(parent);
  label->setMargin(4);
  label->setText(text);
  labels.append(label);
  return label;
}

void LabelAction::deleteWidget(QWidget *widget)
{
  labels.removeAll(dynamic_cast<QLabel *>(widget));
  delete widget;
}

} // namespace internal

// =======================================================================================

MapAirportHandler::MapAirportHandler(QWidget *parent)
  : QObject(parent)
{

}

MapAirportHandler::~MapAirportHandler()
{
  delete toolButton;
}

void MapAirportHandler::saveState()
{
  sliderActionRunwayLength->saveState();
  actionsToFlags();
  atools::settings::Settings::instance().setValueVar(lnm::MAP_AIRPORT, airportTypes.asFlagType());
}

void MapAirportHandler::restoreState()
{
  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_MAP_SETTINGS)
  {
    QVariant defaultValue = static_cast<atools::util::FlagType>(map::AIRPORT_ALL_AND_ADDON);
    airportTypes = atools::settings::Settings::instance().valueVar(lnm::MAP_AIRPORT, defaultValue).value<atools::util::FlagType>();
    sliderActionRunwayLength->restoreState();
  }
  actionEmpty->setEnabled(OptionData::instance().getFlags() & opts::MAP_EMPTY_AIRPORTS);
  runwaySliderValueChanged();
  flagsToActions();
  updateToolbutton();
}

int MapAirportHandler::getMinimumRunwayFt() const
{
  if(sliderActionRunwayLength->getSliderValue() == -1)
    return -1;
  else
    return atools::roundToInt(Unit::rev(sliderActionRunwayLength->getSliderValue() * 100.f, Unit::distShortFeetF));
}

void MapAirportHandler::resetSettingsToDefault()
{
  airportTypes = map::AIRPORT_ALL_AND_ADDON;
  sliderActionRunwayLength->reset();
  flagsToActions();
  runwaySliderValueChanged();
}

void MapAirportHandler::optionsChanged()
{
  actionEmpty->setEnabled(OptionData::instance().getFlags() & opts::MAP_EMPTY_AIRPORTS);
  sliderActionRunwayLength->optionsChanged();
  sliderActionRunwayLength->reset();
  runwaySliderValueChanged();
}

void MapAirportHandler::addToolbarButton()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  toolButton = new QToolButton(ui->toolbarMapOptions);

  // Connect master switch button
  connect(ui->actionMapShowAirports, &QAction::toggled, this, &MapAirportHandler::toolbarActionTriggered);

  // Create and add toolbar button =====================================
  toolButton->setIcon(QIcon(":/littlenavmap/resources/icons/airportmenu.svg"));
  toolButton->setPopupMode(QToolButton::InstantPopup);
  toolButton->setToolTip(tr("Select airport types to show"));
  toolButton->setStatusTip(toolButton->toolTip());
  toolButton->setCheckable(true);

  // Add tear off menu to button =======
  toolButton->setMenu(new QMenu(toolButton));
  QMenu *buttonMenu = toolButton->menu();
  buttonMenu->setToolTipsVisible(true);
  buttonMenu->setTearOffEnabled(true);

  ui->toolbarMapOptions->insertWidget(ui->actionMapShowVor, toolButton);
  ui->toolbarMapOptions->insertSeparator(ui->actionMapShowVor);

  // Create and add actions to toolbar and menu =================================
  actionAll = new QAction(tr("&All"), buttonMenu);
  actionAll->setToolTip(tr("Show all airport types"));
  actionAll->setStatusTip(actionAll->toolTip());
  buttonMenu->addAction(actionAll);
  ui->menuViewAirport->addAction(actionAll);
  connect(actionAll, &QAction::triggered, this, &MapAirportHandler::actionAllTriggered);

  actionNone = new QAction(tr("&None"), buttonMenu);
  actionNone->setToolTip(tr("Hide all airport types"));
  actionNone->setStatusTip(actionNone->toolTip());
  buttonMenu->addAction(actionNone);
  ui->menuViewAirport->addAction(actionNone);
  connect(actionNone, &QAction::triggered, this, &MapAirportHandler::actionNoneTriggered);

  ui->menuViewAirport->addSeparator();
  buttonMenu->addSeparator();

  // actionMapShowAirports Strg+Alt+H
  actionHard = addAction(":/littlenavmap/resources/icons/airport.svg", tr("&Hard surface"),
                         tr("Show airports with at least one hard surface runway"), QKeySequence(tr("Ctrl+Alt+J")));
  actionSoft = addAction(":/littlenavmap/resources/icons/airportsoft.svg", tr("&Soft surface"),
                         tr("Show airports with soft runway surfaces only"), QKeySequence(tr("Ctrl+Alt+S")));
  actionWater = addAction(":/littlenavmap/resources/icons/airportwater.svg", tr("&Water"),
                          tr("Show airports with water runways only"), QKeySequence(tr("Ctrl+Alt+U")));
  actionHelipad = addAction(":/littlenavmap/resources/icons/airporthelipad.svg", tr("&Heliports"),
                            tr("Show airports having only helipads"), QKeySequence(tr("Ctrl+Alt+X")));
  actionEmpty = addAction(":/littlenavmap/resources/icons/airportempty.svg", tr("&Empty"),
                          tr("Show airports having no special features"), QKeySequence(tr("Ctrl+Alt+E")));
  actionUnlighted = addAction(":/littlenavmap/resources/icons/airportlight.svg", tr("&Not lighted"),
                              tr("Show unlighted airports"), QKeySequence());
  actionNoProcedures = addAction(":/littlenavmap/resources/icons/airportproc.svg", tr("&No procedure"),
                                 tr("Show airports having no approach procedure"), QKeySequence());

  toolButton->menu()->addSeparator();
  actionAddon = addAction(":/littlenavmap/resources/icons/airportaddon.svg", tr("&Add-on"),
                          tr("Force visibility of add-on airports for all zoom distances"), QKeySequence(tr("Ctrl+Alt+O")));

  // Create and add the wrapped actions ================
  buttonMenu->addSeparator();
  labelActionRunwayLength = new internal::LabelAction(toolButton->menu());
  toolButton->menu()->addAction(labelActionRunwayLength);
  sliderActionRunwayLength = new internal::SliderAction(toolButton->menu());
  toolButton->menu()->addAction(sliderActionRunwayLength);

  connect(sliderActionRunwayLength, &internal::SliderAction::valueChanged, this, &MapAirportHandler::runwaySliderValueChanged);
  connect(sliderActionRunwayLength, &internal::SliderAction::sliderReleased, this, &MapAirportHandler::runwaySliderReleased);
}

QAction *MapAirportHandler::addAction(const QString& icon, const QString& text, const QString& tooltip, const QKeySequence& shortcut)
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  QAction *action = new QAction(QIcon(icon), text, toolButton->menu());
  action->setToolTip(tooltip);
  action->setStatusTip(tooltip);
  action->setCheckable(true);
  action->setShortcut(shortcut);

  // Add to button and menu
  toolButton->menu()->addAction(action);
  ui->menuViewAirport->addAction(action);

  connect(action, &QAction::triggered, this, &MapAirportHandler::toolbarActionTriggered);

  return action;
}

void MapAirportHandler::actionAllTriggered()
{
  // Save and restore master airport flag
  bool airportFlag = airportTypes.testFlag(map::AIRPORT);
  airportTypes = map::AIRPORT_ALL_AND_ADDON;
  airportTypes.setFlag(map::AIRPORT, airportFlag);

  flagsToActions();
  sliderActionRunwayLength->reset();
  runwaySliderValueChanged();
  updateToolbutton();
  emit updateAirportTypes();
}

void MapAirportHandler::actionNoneTriggered()
{
  // Save and restore master airport flag
  bool airportFlag = airportTypes.testFlag(map::AIRPORT);
  airportTypes = map::NONE;
  airportTypes.setFlag(map::AIRPORT, airportFlag);

  flagsToActions();
  updateToolbutton();
  emit updateAirportTypes();
}

void MapAirportHandler::toolbarActionTriggered()
{
  actionsToFlags();
  updateToolbutton();
  emit updateAirportTypes();
}

void MapAirportHandler::flagsToActions()
{
  atools::gui::SignalBlocker blocker({NavApp::getMainUi()->actionMapShowAirports, actionHard, actionSoft, actionWater, actionHelipad,
                                      actionAddon, actionUnlighted, actionNoProcedures, actionEmpty});

  NavApp::getMainUi()->actionMapShowAirports->setChecked(airportTypes.testFlag(map::AIRPORT));
  actionHard->setChecked(airportTypes.testFlag(map::AIRPORT_HARD));
  actionSoft->setChecked(airportTypes.testFlag(map::AIRPORT_SOFT));
  actionWater->setChecked(airportTypes.testFlag(map::AIRPORT_WATER));
  actionHelipad->setChecked(airportTypes.testFlag(map::AIRPORT_HELIPAD));
  actionAddon->setChecked(airportTypes.testFlag(map::AIRPORT_ADDON));
  actionUnlighted->setChecked(airportTypes.testFlag(map::AIRPORT_UNLIGHTED));
  actionNoProcedures->setChecked(airportTypes.testFlag(map::AIRPORT_NO_PROCS));
  actionEmpty->setChecked(airportTypes.testFlag(map::AIRPORT_EMPTY));
}

void MapAirportHandler::actionsToFlags()
{
  airportTypes = map::NONE;

  if(NavApp::getMainUi()->actionMapShowAirports->isChecked())
    airportTypes |= map::AIRPORT;

  if(actionHard->isChecked())
    airportTypes |= map::AIRPORT_HARD;

  if(actionSoft->isChecked())
    airportTypes |= map::AIRPORT_SOFT;

  if(actionWater->isChecked())
    airportTypes |= map::AIRPORT_WATER;

  if(actionHelipad->isChecked())
    airportTypes |= map::AIRPORT_HELIPAD;

  if(actionAddon->isChecked())
    airportTypes |= map::AIRPORT_ADDON;

  if(actionUnlighted->isChecked())
    airportTypes |= map::AIRPORT_UNLIGHTED;

  if(actionNoProcedures->isChecked())
    airportTypes |= map::AIRPORT_NO_PROCS;

  if(actionEmpty->isChecked())
    airportTypes |= map::AIRPORT_EMPTY;
}

void MapAirportHandler::runwaySliderValueChanged()
{
  updateToolbutton();
  updateRunwayLabel();
  emit updateAirportTypes();
}

void MapAirportHandler::runwaySliderReleased()
{
  updateToolbutton();
  emit updateAirportTypes();
}

void MapAirportHandler::updateToolbutton()
{
  toolButton->setChecked(airportTypes & (map::MapTypes(map::AIRPORT_ALL_AND_ADDON) & ~map::MapTypes(map::AIRPORT)));
}

void MapAirportHandler::updateRunwayLabel()
{
  int runwayLength = getMinimumRunwayFt();
  labelActionRunwayLength->setText(runwayLength > 0 ?
                                   tr("Min. runway length %1.").arg(Unit::distShortFeet(runwayLength)) :
                                   tr("No runway limit."));
}
