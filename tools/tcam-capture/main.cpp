/*
 * Copyright 2021 The Imaging Source Europe GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <gst/gst.h>

int main(int argc, char* argv[])
{
    // always init gstreamer
    gst_init(&argc, &argv);

    // make logging useful
    qSetMessagePattern("%{file}(%{line}): %{message}");

    QApplication a(argc, argv);

    a.setOrganizationName("the_imaging_source");
    a.setOrganizationDomain("theimagingsource.com");
    a.setApplicationName("tcam-capture");
    // TODO get from version.h
    a.setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("The Imaging Source Live Stream Application");
    parser.addHelpOption();
    parser.addVersionOption();

    // A boolean option with a single name (-p)
    QCommandLineOption reset_option("reset", "Reset application settings and clear cache");
    parser.addOption(reset_option);

    //QCommandLineOption config_option("config", "Use custom config");
    parser.addPositionalArgument("config", "Use custom config");

    // Process the actual command line arguments given by the user
    parser.process(a);

    const QStringList args = parser.positionalArguments();

    for (int i = 0; i < args.size(); i++)
    {
        qInfo("%s", args.at(i).toStdString().c_str());
    }
    MainWindow w;

    w.show();
    return a.exec();
}
