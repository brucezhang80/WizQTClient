#include "wizkmsync.h"

#include <QDebug>
#include <QApplication>

#include "apientry.h"
#include "token.h"

#include "../share/wizDatabase.h"
#include "sync_p.h"


/* ---------------------------- CWizKMSyncThead ---------------------------- */
void CWizKMSyncEvents::OnSyncProgress(int pos)
{
    Q_UNUSED(pos);
}

HRESULT CWizKMSyncEvents::OnText(WizKMSyncProgressMessageType type, const QString& strStatus)
{
    Q_UNUSED(type);
    qInfo() << "[Sync]"  << strStatus;

    Q_EMIT messageReady(strStatus);
    return 0;
}

HRESULT CWizKMSyncEvents::OnMessage(WizKMSyncProgressMessageType type, const QString& strTitle, const QString& strMessage)
{
    emit promptMessageRequest(type, strTitle, strMessage);
    return S_OK;
}

HRESULT CWizKMSyncEvents::OnBubbleNotification(const QVariant& param)
{
    emit bubbleNotificationRequest(param);
    return S_OK;
}

void CWizKMSyncEvents::SetDatabaseCount(int count)
{
    OnStatus(QObject::tr("Set database count: %1").arg(count));
}

void CWizKMSyncEvents::SetCurrentDatabase(int index)
{
    OnStatus(QObject::tr("Set current database index: %1").arg(index));
}

void CWizKMSyncEvents::ClearLastSyncError(IWizSyncableDatabase* pDatabase)
{
    // FIXME
    Q_UNUSED(pDatabase);
}

void CWizKMSyncEvents::OnTrafficLimit(IWizSyncableDatabase* pDatabase)
{
    // FIXME
    Q_UNUSED(pDatabase);
}

void CWizKMSyncEvents::OnStorageLimit(IWizSyncableDatabase* pDatabase)
{
    // FIXME
    Q_UNUSED(pDatabase);
}

void CWizKMSyncEvents::OnBizServiceExpr(IWizSyncableDatabase *pDatabase)
{
    // FIXME
    Q_UNUSED(pDatabase);
}

void CWizKMSyncEvents::OnBizNoteCountLimit(IWizSyncableDatabase* pDatabase)
{
    // FIXME
    Q_UNUSED(pDatabase);
}

void CWizKMSyncEvents::OnUploadDocument(const QString& strDocumentGUID, bool bDone)
{
    if (bDone)
    {
        OnStatus(QObject::tr("Upload document: %1 finished").arg(strDocumentGUID));
    }
    else
    {
        OnStatus(QObject::tr("Upload document: %1 start").arg(strDocumentGUID));
    }
}

void CWizKMSyncEvents::OnBeginKb(const QString& strKbGUID)
{
    OnStatus(QObject::tr("OnBeginKb kb_guid: %1").arg(strKbGUID));
}

void CWizKMSyncEvents::OnEndKb(const QString& strKbGUID)
{
    OnStatus(QObject::tr("OnEndKb kb_guid: %1").arg(strKbGUID));
}

/* ---------------------------- CWizKMSyncThead ---------------------------- */

#define DEFAULT_FULL_SYNC_SECONDS_INTERVAL 15 * 60
#define DEFAULT_QUICK_SYNC_MILLISECONDS_INTERVAL 1000

static CWizKMSyncThread* g_pSyncThread = NULL;
CWizKMSyncThread::CWizKMSyncThread(CWizDatabase& db, QObject* parent)
    : QThread(parent)
    , m_db(db)
    , m_bNeedSyncAll(false)
    , m_bNeedDownloadMessages(false)
    , m_pEvents(NULL)
    , m_bBackground(true)
    , m_nFullSyncSecondsInterval(DEFAULT_FULL_SYNC_SECONDS_INTERVAL)
    , m_bBusy(false)
    , m_bPause(false)
{
    m_tLastSyncAll = QDateTime::currentDateTime();
    //
    m_pEvents = new CWizKMSyncEvents();
    //
    connect(m_pEvents, SIGNAL(messageReady(const QString&)), SIGNAL(processLog(const QString&)));
    connect(m_pEvents, SIGNAL(promptMessageRequest(int, QString, QString)), SIGNAL(promptMessageRequest(int, QString, QString)));
    connect(m_pEvents, SIGNAL(bubbleNotificationRequest(const QVariant&)), SIGNAL(bubbleNotificationRequest(const QVariant&)));

    m_timer.setSingleShot(true);
    connect(this, SIGNAL(startTimer(int)), &m_timer, SLOT(start(int)));
    connect(this, SIGNAL(stopTimer()), &m_timer, SLOT(stop()));
    connect(&m_timer, SIGNAL(timeout()), SLOT(on_timerOut()));
    //
    g_pSyncThread = this;
}
CWizKMSyncThread::~CWizKMSyncThread()
{
    g_pSyncThread = NULL;
}

void CWizKMSyncThread::run()
{
    while (!m_pEvents->IsStop())
    {
        m_mutex.lock();
        m_wait.wait(&m_mutex, 1000 * 3);
        m_mutex.unlock();

        if (m_pEvents->IsStop())
        {
            return;
        }
        //
        if (m_bPause)
            continue;
        //
        m_bBusy = true;
        doSync();
        m_bBusy = false;
    }
}

void CWizKMSyncThread::syncAfterStart()
{
#ifndef QT_DEBUG
    if (m_tLastSyncAll.secsTo(QDateTime::currentDateTime()) < 5)
        return;

    startSyncAll(false);
#endif
}

void CWizKMSyncThread::on_timerOut()
{
    m_mutex.lock();
    m_wait.wakeAll();
    m_mutex.unlock();
}

void CWizKMSyncThread::startSyncAll(bool bBackground)
{
    m_mutex.lock();
    m_bNeedSyncAll = true;
    m_bBackground = bBackground;

    m_wait.wakeAll();
    m_mutex.unlock();
}

bool CWizKMSyncThread::isBackground() const
{
    return m_bBackground;
}


bool CWizKMSyncThread::prepareToken()
{
    QString token = Token::token();
    if (token.isEmpty())
    {
        Q_EMIT syncFinished(Token::lastErrorCode(), Token::lastErrorMessage(), isBackground());
        return false;
    }
    //
    m_info = Token::info();
    //
    return true;
}

bool CWizKMSyncThread::doSync()
{
    if (needSyncAll())
    {
        qDebug() << "[Sync] syncing all started, thread:" << QThread::currentThreadId();

        syncAll();
        m_bNeedSyncAll = false;
        m_tLastSyncAll = QDateTime::currentDateTime();
        emit startTimer(m_nFullSyncSecondsInterval * 1000 + 1);
        return true;
    }
    else if (needQuickSync())
    {
        qDebug() << "[Sync] quick syncing started, thread:" << QThread::currentThreadId();
        //
        m_bBackground = true;
        quickSync();
        return true;
    }
    else if (needDownloadMessage())
    {
        qDebug() <<  "[Sync] quick download messages started, thread:" << QThread::currentThreadId();
        //
        downloadMesages();
        return true;
    }
    //
    return false;
}

bool CWizKMSyncThread::clearCurrentToken()
{
    Token::clearToken();
    Token::clearLastError();
    return true;
}

void CWizKMSyncThread::waitForDone()
{
    stopSync();
    //
    ::WizWaitForThread(this);
}

bool CWizKMSyncThread::needSyncAll()
{
    if (m_bNeedSyncAll)
        return true;

#ifdef QT_DEBUG
    return false;
#endif

    QDateTime tNow = QDateTime::currentDateTime();
    int seconds = m_tLastSyncAll.secsTo(tNow);
    if (m_nFullSyncSecondsInterval > 0 && seconds > m_nFullSyncSecondsInterval)
    {
        m_bNeedSyncAll = true;
        m_bBackground = true;
    }

    return m_bNeedSyncAll;
}


class CWizKMSyncThreadHelper
{
    CWizKMSyncThread* m_pThread;
public:
    CWizKMSyncThreadHelper(CWizKMSyncThread* pThread, bool syncAll)
        :m_pThread(pThread)
    {
        Q_EMIT m_pThread->syncStarted(syncAll);
    }
    ~CWizKMSyncThreadHelper()
    {
        Q_EMIT m_pThread->syncFinished(m_pThread->m_pEvents->GetLastErrorCode()
                                       , m_pThread->m_pEvents->GetLastErrorMessage()
                                       , m_pThread->isBackground());
        m_pThread->m_pEvents->ClearLastErrorMessage();
    }
};

bool CWizKMSyncThread::syncAll()
{
    m_bNeedSyncAll = false;
    //
    CWizKMSyncThreadHelper helper(this, true);
    Q_UNUSED(helper);
    //
    m_pEvents->SetLastErrorCode(0);
    if (!prepareToken())
        return false;

    if (m_db.kbGUID().isEmpty()) {
        m_db.setKbGUID(Token::info().strKbGUID);
    }

    syncUserCert();

    ::WizSyncDatabase(m_info, m_pEvents, &m_db, m_bBackground);

    return true;
}


bool CWizKMSyncThread::quickSync()
{
    CWizKMSyncThreadHelper helper(this, false);
    //
    Q_UNUSED(helper);
    //
    QString kbGuid;
    while (peekQuickSyncKb(kbGuid))
    {
        if (!prepareToken())
            return false;

        if (kbGuid.isEmpty() || m_db.kbGUID() == kbGuid)
        {
            CWizKMSync syncPrivate(&m_db, m_info, m_pEvents, FALSE, TRUE, NULL);
            //
            if (syncPrivate.Sync())
            {
                m_db.SaveLastSyncTime();
            }
        }
        else
        {
            WIZGROUPDATA group;
            if (m_db.GetGroupData(kbGuid, group))
            {
                IWizSyncableDatabase* pGroupDatabase = m_db.GetGroupDatabase(group);
                //
                WIZUSERINFO userInfo = m_info;
                userInfo.strKbGUID = group.strGroupGUID;
                userInfo.strDatabaseServer = group.strDatabaseServer;
                if (userInfo.strDatabaseServer.isEmpty())
                {
                    userInfo.strDatabaseServer = CommonApiEntry::kUrlFromGuid(userInfo.strToken, userInfo.strKbGUID);
                }
                //
                CWizKMSync syncGroup(pGroupDatabase, userInfo, m_pEvents, TRUE, TRUE, NULL);
                //
                if (syncGroup.Sync())
                {
                    pGroupDatabase->SaveLastSyncTime();
                }
                //
                m_db.CloseGroupDatabase(pGroupDatabase);
            }
        }
    }
    //
    //
    return true;
}

bool CWizKMSyncThread::downloadMesages()
{
    if (!prepareToken())
        return false;

    ::WizQuickDownloadMessage(m_info, m_pEvents, &m_db);

    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    m_bNeedDownloadMessages = false;
    return true;
}



// FIXME: remove this to syncing flow
void CWizKMSyncThread::syncUserCert()
{
    QString strN, stre, strd, strHint;

    CWizKMAccountsServer serser(CommonApiEntry::syncUrl());
    if (serser.GetCert(m_db.GetUserId(), m_db.GetPassword(), strN, stre, strd, strHint)) {
        m_db.SetUserCert(strN, stre, strd, strHint);
    }
}

bool CWizKMSyncThread::needQuickSync()
{
    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    //
    if (m_setQuickSyncKb.empty())
        return false;
    //
    return true;
}

bool CWizKMSyncThread::needDownloadMessage()
{
    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    //
    if (m_bNeedDownloadMessages)
    {
        m_bNeedDownloadMessages = false;
        return true;
    }
    return false;
}


void CWizKMSyncThread::stopSync()
{
    if (isRunning() && m_pEvents)
    {
        m_pEvents->SetStop(true);
        m_mutex.lock();
        m_wait.wakeAll();
        m_mutex.unlock();
    }
}

void CWizKMSyncThread::setFullSyncInterval(int nMinutes)
{
    m_nFullSyncSecondsInterval = nMinutes * 60;
}

void CWizKMSyncThread::addQuickSyncKb(const QString& kbGuid)
{
    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    //
    m_setQuickSyncKb.insert(kbGuid);
    //
    m_tLastKbModified = QDateTime::currentDateTime();

    QTimer::singleShot(DEFAULT_QUICK_SYNC_MILLISECONDS_INTERVAL + 1, this, SLOT(on_timerOut()));
}

void CWizKMSyncThread::quickDownloadMesages()
{
    // 一分钟中内不重复查询，防止过于频繁的请求
    static QTime time = QTime::currentTime().addSecs(-61);
    if (time.secsTo(QTime::currentTime()) < 60)
        return;
    time = QTime::currentTime();

    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    m_bNeedDownloadMessages = true;
    m_wait.wakeAll();
}

bool CWizKMSyncThread::peekQuickSyncKb(QString& kbGuid)
{
    QMutexLocker locker(&m_mutex);
    Q_UNUSED(locker);
    //
    if (m_setQuickSyncKb.empty())
        return false;
    //
    kbGuid = *m_setQuickSyncKb.begin();
    m_setQuickSyncKb.erase(m_setQuickSyncKb.begin());
    return true;
}
void CWizKMSyncThread::quickSyncKb(const QString& kbGuid)
{
    if (!g_pSyncThread)
        return;
    //
    g_pSyncThread->addQuickSyncKb(kbGuid);
}
bool CWizKMSyncThread::isBusy()
{
    if (!g_pSyncThread)
        return false;
    //
    return g_pSyncThread->m_bBusy;
}

void CWizKMSyncThread::waitUntilIdleAndPause()
{
    while(isBusy())
    {
        QThread::sleep(1);
    }
    //
    setPause(true);
}

void CWizKMSyncThread::setPause(bool pause)
{
    if (!g_pSyncThread)
        return;
    //
    g_pSyncThread->m_bPause = pause;
}
