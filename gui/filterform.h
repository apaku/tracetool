/**********************************************************************
** Copyright (C) 2010 froglogic GmbH.
** All rights reserved.
**********************************************************************/

#ifndef FILTERFORM_H
#define FILTERFORM_H

#include <QWidget>
#include "ui_filterform.h"
#include "settings.h"

class FilterForm : public QWidget, private Ui::FilterForm
{
    Q_OBJECT
public:
    explicit FilterForm(Settings *settings, QWidget *parent = 0);

    void setTraceKeys(const QStringList &traceKeys);

signals:
    void addNewTraceKey(const QString &key);
    void filterApplied();

public slots:
    void apply();

    void restoreSettings();

private slots:
    void addTraceKey();

private:
    void saveSettings();

    Settings* const m_settings;
};

#endif

