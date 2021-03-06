#include "parserbridge.h"
#include <QDir>
#include <QMessageBox>
#include <QProcess>
#include "accessmanager.h"
#include "danmakudelaygetter.h"
#include "downloader.h"
#include "platforms.h"
#include "playlist.h"
#include "reslibrary.h"
#include "selectiondialog.h"
#include "settings_network.h"
#include "settings_plugins.h"
#include "terminal.h"
#include "ykdlbridge.h"
#include "yougetbridge.h"

SelectionDialog *ParserBridge::selectionDialog = NULL;

ParserBridge::ParserBridge(QObject *parent) : QObject(parent)
{
    process = new QProcess(this);

    // Set environments
    QStringList envs = QProcess::systemEnvironment();
    envs << "PYTHONIOENCODING=utf8";
#ifdef Q_OS_MAC
    envs << "LC_CTYPE=en_US.UTF-8";
    // add "/usr/local/bin" to path
    for (int i = 0; i < envs.size(); i++)
    {
        if (envs[i].startsWith("PATH=") && !envs[i].contains("/usr/local/bin"))
        {
            envs[i] += ":/usr/local/bin";
            break;
        }
    }
#endif
    process->setEnvironment(envs);
    connect(process, SIGNAL(finished(int)),this, SLOT(onFinished()));
    connect(process, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onError()));
}


ParserBridge::~ParserBridge()
{
    if (process->state() == QProcess::Running)
    {
        process->kill();
        process->waitForFinished();
    }
}


void ParserBridge::parse(const QString &url, bool download)
{
    if (process->state() == QProcess::Running)
    {
        QMessageBox::warning(NULL, "Error", tr("Another file is being parsed."));
        return;
    }
    this->url = url;
    this->download = download;
    runParser(url);
}


void ParserBridge::onFinished()
{
    if (selectionDialog == NULL)
        selectionDialog = new SelectionDialog;
    QByteArray output = process->readAllStandardOutput();
    title.clear();
    container.clear();
    danmaku_url.clear();
    names.clear();
    urls.clear();
    referer.clear();
    ua.clear();
    seekable = true;
    is_dash = false;
    parseOutput(output);

    // Error
    if (urls.isEmpty())
    {
        onError();
        return;
    }

    // Bind referer and use-agent
    if (!referer.isEmpty())
    {
        foreach (QString url, urls)
            referer_table[QUrl(url).host()] = referer.toUtf8();
    }
    if (!ua.isEmpty())
    {
        foreach (QString url, urls)
            ua_table[QUrl(url).host()] = ua.toUtf8();
    }
    if (!seekable)
    {
         foreach (QString url, urls) {
            unseekable_hosts.append(QUrl(url).host());
        }
    }

    // Download
    if (download)
    {
        // Build file path list
        QDir dir = QDir(Settings::downloadDir);
        QString dirname = title + '.' + container;
        if (urls.size() > 1)
        {
            if (!dir.cd(dirname))
            {
                dir.mkdir(dirname);
                dir.cd(dirname);
            }
        }
        for (int i = 0; i < names.size(); i++)
             names[i] = dir.filePath(QString(names[i]));

        // Download videos with danmaku
        if (!danmaku_url.isEmpty())
        {
            if (urls.size() > 1)
                new DanmakuDelayGetter(names, urls, danmaku_url, true, this);
            else
                downloader->addTask(urls[0].toUtf8(), names[0], false, danmaku_url.toUtf8());
        }
        // Download videos without danmaku
        else
        {
            for (int i = 0; i < urls.size(); i++)
                 downloader->addTask(urls[i].toUtf8(), names[i], urls.size() > 1);
        }
        QMessageBox::information(NULL, "Message", tr("Add download task successfully!"));
    }

    // Play
    else if (is_dash) // dash streams
    {
        playlist->addFileAndPlay(title, urls[0], 0, urls[1]);
        res_library->close();
    }
    else if (!danmaku_url.isEmpty()) // with danmaku
    {
        if (urls.size() > 1)
            new DanmakuDelayGetter(names, urls, danmaku_url, false, this);
        else
            playlist->addFileAndPlay(names[0], urls[0], danmaku_url);
    }
    else
    {
        playlist->addFileAndPlay(names[0], urls[0]);
        for (int i = 1; i < urls.size(); i++)
            playlist->addFile(names[i], urls[i]);
        res_library->close();
    }
}

void ParserBridge::onError()
{
    // Use fallback parser
    ParserBridge *firstParser, *secondParser;
    QString msg;
    if (Settings::parser == Settings::YKDL)
    {
        firstParser = &ykdl_bridge;
        secondParser = &you_get_bridge;
        msg = tr("Parsing with ykdl failed. We will try with you-get again.");
    }
    else
    {
        firstParser = &you_get_bridge;
        secondParser = &ykdl_bridge;
        msg = tr("Parsing with you-get failed. We will try with ykdl again.");
    }

    if (this == firstParser)
    {
        QMessageBox::warning(NULL, "Error",
                             msg + "\n\nURL:" + url + "\n\nError Output:\n" +
                             QString::fromUtf8(process->readAllStandardError()));
        secondParser->parse(url, download);
    }

    // parse with fallback parser failed
    else
    {
        int btn = QMessageBox::warning(NULL, "Error",
                                       "Parse failed!\nURL:" + url + "\n" +
                                       QString::fromUtf8(process->readAllStandardError()),
                                       tr("Cancel"),
                                       tr("Upgrade parser"));
        if (btn == 1)
            upgradeParsers();
    }
}



void ParserBridge::upgradeParsers()
{
    execShell(parserUpgraderPath());
}

