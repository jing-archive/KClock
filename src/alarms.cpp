/*
 * Copyright 2020 Devin Lin <espidev@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QQmlEngine>
#include <QTime>
#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KNotification>
#include <KSharedConfig>

#include "alarms.h"

const QString ALARM_CFG_GROUP = "Alarms";

Alarm::Alarm(QObject *parent, QString name, int minutes, int hours, int dayOfWeek)
    : QObject(parent)
{
    enabled = true;
    uuid = QUuid::createUuid();
    this->name = name;
    this->minutes = minutes;
    this->hours = hours;
    this->dayOfWeek = dayOfWeek;
    this->lastAlarm = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    ringtonePlayer = new QMediaPlayer();
}

// alarm from json
Alarm::Alarm(QString serialized)
{
    if (serialized == "") {
        uuid = QUuid::createUuid();
    } else {
        QJsonDocument doc = QJsonDocument::fromJson(serialized.toUtf8());
        QJsonObject obj = doc.object();
        uuid = QUuid::fromString(obj["uuid"].toString());
        name = obj["name"].toString();
        minutes = obj["minutes"].toInt();
        hours = obj["hours"].toInt();
        dayOfWeek = obj["dayOfWeek"].toInt();
        enabled = obj["enabled"].toBool();
        lastAlarm = obj["lastAlarm"].toInt();
        snooze = obj["snooze"].toInt();
        lastSnooze = obj["lastSnooze"].toInt();
    }
}

// alarm to json
QString Alarm::serialize()
{
    QJsonObject obj;
    obj["uuid"] = this->uuid.toString();
    obj["name"] = this->name;
    obj["minutes"] = this->minutes;
    obj["hours"] = this->hours;
    obj["dayOfWeek"] = this->dayOfWeek;
    obj["enabled"] = this->enabled;
    obj["lastAlarm"] = this->lastAlarm;
    obj["snooze"] = this->snooze;
    obj["lastSnooze"] = this->lastSnooze;
    return QString(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Alarm::save()
{
    auto config = KSharedConfig::openConfig();
    KConfigGroup group = config->group(ALARM_CFG_GROUP);
    group.writeEntry(this->getUuid().toString(), this->serialize());
    group.sync();
}

void Alarm::ring()
{
    qDebug("Found alarm to run, sending notification...");

    //    KNotification *notif = new KNotification("timerFinished");
    //    notif->setActions(QStringList() << "Dismiss"
    //                                    << "Snooze");
    //    notif->setIconName("kronometer");
    //    notif->setTitle(this->getName());
    //    notif->setText(QDateTime::currentDateTime().toLocalTime().toString("hh:mm ap")); // TODO
    //    notif->setDefaultAction(i18n("View"));
    //    notif->setUrgency(KNotification::HighUrgency);
    //    notif->setFlags(KNotification::NotificationFlag::LoopSound | KNotification::NotificationFlag::Persistent);

    //    connect(notif, &KNotification::defaultActivated, this, &Alarm::handleDismiss);
    //    connect(notif, &KNotification::action1Activated, this, &Alarm::handleDismiss);
    //    connect(notif, &KNotification::action2Activated, this, &Alarm::handleSnooze);

    //    notif->sendEvent();
}

void Alarm::handleDismiss()
{
    qDebug() << "Alarm dismissed";
    this->setLastSnooze(0);
    this->save();
}

void Alarm::handleSnooze()
{
    qDebug() << "Alarm snoozed (5 minutes)" << lastSnooze;
    this->setSnooze(this->lastSnooze + 60 * 5); // snooze 5 minutes
    this->setLastSnooze(this->snooze);
    this->setEnabled(true);
    this->save();
    emit onPropertyChanged();
}

qint64 Alarm::toPreviousAlarm(qint64 timestamp)
{
    QDateTime date = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime(); // local time
    QTime alarmTime = QTime(this->getHours(), this->getMinutes());

    if (this->getDayOfWeek() == 0) {    // no repeat alarm
        if (alarmTime <= date.time()) { // current day
            return QDateTime(date.date(), alarmTime).toSecsSinceEpoch();
        } else { // previous day
            return QDateTime(date.addDays(-1).date(), alarmTime).toSecsSinceEpoch();
        }
    } else { // repeat alarm
        bool first = true;

        // keeping going back a day until the day of week is accepted
        while (((this->getDayOfWeek() & (1 << (date.date().dayOfWeek() - 1))) == 0) // check day
               || (first && (alarmTime > date.time())))                             // check time on first day
        {
            date = date.addDays(-1); // go back a day
            first = false;
        }

        return QDateTime(date.date(), alarmTime).toSecsSinceEpoch();
    }
}

bool Alarm::selectRingtone()
{
    QFileDialog::getOpenFileName(nullptr, tr("Open Image"), "/home/jana", tr("Image Files (*.png *.jpg *.bmp)"));
    return true;
}

Alarm::~Alarm()
{
    delete ringtonePlayer;
}

/* ~~~ Alarm Model ~~~ */

AlarmModel::AlarmModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // add alarms from config
    auto config = KSharedConfig::openConfig();
    KConfigGroup group = config->group(ALARM_CFG_GROUP);
    for (QString key : group.keyList()) {
        QString json = group.readEntry(key, "");
        if (json != "")
            alarmsList.append(new Alarm(json));
    }

    // start alarm timer
    this->timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, QOverload<>::of(&AlarmModel::checkAlarmsToRun));
    timer->start(2000);
}

void AlarmModel::checkAlarmsToRun()
{
    qint64 curTime = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();

    for (Alarm *alarm : alarmsList) {
        if (alarm == nullptr || !alarm->isEnabled())
            continue;

        // if it is time for alarm to ring
        if (alarm->toPreviousAlarm(alarm->getLastAlarm()) < alarm->toPreviousAlarm(curTime) ||                                                  // is next cycle
            (alarm->getSnooze() != 0 && alarm->toPreviousAlarm(alarm->getLastAlarm()) == alarm->toPreviousAlarm(curTime - alarm->getSnooze()))) // snooze
        {
            alarm->setSnooze(0);

            // ring alarm and set last time the alarm rang
            if (60 * 5 > curTime - alarm->toPreviousAlarm(curTime)) // only ring if it has been within 5 minutes of the alarm time
                alarm->ring();

            alarm->setLastAlarm(curTime);

            // disable alarm if run once
            if (alarm->getDayOfWeek() == 0)
                alarm->setEnabled(false);

            // save alarm after run
            alarm->save();
            updateUi();
        }
    }
}

/* ~ Alarm row data ~ */

QHash<int, QByteArray> AlarmModel::roleNames() const
{
    return {
        {HoursRole, "hours"},
        {MinutesRole, "minutes"},
        {NameRole, "name"},
        {EnabledRole, "enabled"},
        {DayOfWeekRole, "dayOfWeek"},
    };
}

QVariant AlarmModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= alarmsList.count())
        return QVariant();

    auto *alarm = alarmsList[index.row()];
    if (alarm == nullptr)
        return false;
    if (role == EnabledRole)
        return alarm->isEnabled();
    else if (role == HoursRole)
        return alarm->getHours();
    else if (role == MinutesRole)
        return alarm->getMinutes();
    else if (role == NameRole)
        return alarm->getName();
    else if (role == DayOfWeekRole)
        return alarm->getDayOfWeek();
    else
        return QVariant();
}

bool AlarmModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || alarmsList.length() <= index.row())
        return false;

    auto *alarm = alarmsList[index.row()];
    if (alarm == nullptr)
        return false;
    if (role == EnabledRole)
        alarm->setEnabled(value.toBool());
    else if (role == HoursRole)
        alarm->setHours(value.toInt());
    else if (role == MinutesRole)
        alarm->setMinutes(value.toInt());
    else if (role == NameRole)
        alarm->setName(value.toString());
    else if (role == DayOfWeekRole)
        alarm->setDayOfWeek(value.toInt());
    else
        return false;

    alarm->save();

    emit dataChanged(index, index);
    return true;
}

int AlarmModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return alarmsList.size();
}

Qt::ItemFlags AlarmModel::flags(const QModelIndex &index) const
{
    Q_UNUSED(index);
    return Qt::ItemIsEditable;
}

Alarm *AlarmModel::insert(int index, QString name, int minutes, int hours, int dayOfWeek)
{
    if (index < 0 || index > alarmsList.count())
        return new Alarm();
    emit beginInsertRows(QModelIndex(), index, index);

    auto *alarm = new Alarm(this, name, minutes, hours, dayOfWeek);
    QQmlEngine::setObjectOwnership(alarm, QQmlEngine::CppOwnership); // prevent segfaults from js garbage collecting
    alarmsList.insert(index, alarm);

    // write to config
    alarm->save();

    emit endInsertRows();
    return alarm;
}

void AlarmModel::remove(int index)
{
    if (index < 0 || index >= alarmsList.count())
        return;
    emit beginRemoveRows(QModelIndex(), index, index);

    // write to config
    auto config = KSharedConfig::openConfig();
    KConfigGroup group = config->group(ALARM_CFG_GROUP);
    group.deleteEntry(alarmsList.at(index)->getUuid().toString());

    alarmsList.removeAt(index);

    emit endRemoveRows();
}

Alarm *AlarmModel::get(int index)
{
    if (index < 0 || index >= alarmsList.count())
        return new Alarm();
    return alarmsList.at(index);
}

void AlarmModel::updateUi()
{
    emit dataChanged(createIndex(0, 0), createIndex(alarmsList.count() - 1, 0));
}
