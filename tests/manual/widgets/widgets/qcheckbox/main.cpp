// Copyright (C) 2017 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QMainWindow>

class MyMainWindow : public QMainWindow
{
public:
    MyMainWindow(QWidget *parent = nullptr) : QMainWindow(parent)
    {
        QGridLayout *layout = new QGridLayout;

        auto *label = new QLabel("default");
        layout->addWidget(label, 0, 0);
        auto *checkBox = new QCheckBox;
        layout->addWidget(checkBox, 0, 1);

        label = new QLabel("AlignHCenter");
        layout->addWidget(label, 1, 0);
        checkBox = new QCheckBox;
        checkBox->setAlignment(Qt::AlignHCenter);
        layout->addWidget(checkBox, 1, 1);

        label = new QLabel("AlignRight");
        layout->addWidget(label, 2, 0);
        checkBox = new QCheckBox;
        checkBox->setAlignment(Qt::AlignRight);
        checkBox->setText("right");
        layout->addWidget(checkBox, 2, 1);

        setLayout(layout);
    }
};

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MyMainWindow *w = new MyMainWindow;
    w->show();
    int ret = a.exec();
    return ret;
}

