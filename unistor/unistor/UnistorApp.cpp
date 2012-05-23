#include "UnistorApp.h"
#include "CwxDate.h"
///构造函数
UnistorApp::UnistorApp(){
    m_store = NULL;
    m_recvChannel = NULL;
    m_recvThreadPool = NULL;
    m_recvArgs = NULL;
    m_writeThreadPool = NULL;
    m_masterHandler = NULL;
    m_recvWriteHandler = NULL;
    m_innerSyncThreadPool = NULL;
    m_innerSyncChannel = NULL;
    m_outerSyncThreadPool = NULL;
    m_outerSyncChannel = NULL;
    m_checkpointHandler = NULL;
    m_checkpointThreadPool = NULL;
    m_transThreadPool = NULL;
    m_transChannel = NULL;
    m_zkThreadPool = NULL;
    m_zkHandler = NULL;
}

///析构函数
UnistorApp::~UnistorApp(){
}

///初始化
int UnistorApp::init(int argc, char** argv){
    string strErrMsg;
    ///首先调用架构的init api
    if (CwxAppFramework::init(argc, argv) == -1) return -1;
    ///检查是否通过-f指定了配置文件，若没有，则采用默认的配置文件
    if ((NULL == this->getConfFile()) || (strlen(this->getConfFile()) == 0)){
        this->setConfFile("unistor.cnf");
    }
    ///加载配置文件，若失败则退出
    if (0 != m_config.init(getConfFile())){
        CWX_ERROR((m_config.getErrMsg()));
        return -1;
    }
    ///设置运行日志的输出level
    setLogLevel(CwxLogger::LEVEL_ERROR|CwxLogger::LEVEL_INFO|CwxLogger::LEVEL_WARNING|CwxLogger::LEVEL_DEBUG);
    return 0;
}

///配置运行环境信息
int UnistorApp::initRunEnv(){
    ///设置系统的时钟间隔，最小刻度为1ms，此为1s。
    this->setClick(100);//0.1s
    ///设置工作目录
    this->setWorkDir(m_config.getCommon().m_strWorkDir.c_str());
    ///设置循环运行日志的数量
    this->setLogFileNum(LOG_FILE_NUM);
    ///设置每个日志文件的大小
    this->setLogFileSize(LOG_FILE_SIZE*1024*1024);
    ///调用架构的initRunEnv，使以上设置的参数生效
    if (CwxAppFramework::initRunEnv() == -1 ) return -1;
    ///将加载的配置文件信息输出到日志文件中，以供查看检查
    m_config.outputConfig();
    ///block各种signal
    this->blockSignal(SIGTERM);
    this->blockSignal(SIGUSR1);
    this->blockSignal(SIGUSR2);
    this->blockSignal(SIGCHLD);
    this->blockSignal(SIGCLD);
    this->blockSignal(SIGHUP);
    this->blockSignal(SIGPIPE);
    this->blockSignal(SIGALRM);
    this->blockSignal(SIGCONT);
    this->blockSignal(SIGSTOP);
    this->blockSignal(SIGTSTP);
    this->blockSignal(SIGTTOU);

    //set version
    this->setAppVersion(UNISTOR_VERSION);
    //set last modify date
    this->setLastModifyDatetime(UNISTOR_MODIFY_DATE);
    //set compile date
    this->setLastCompileDatetime(CWX_COMPILE_DATE(_BUILD_DATE));
    ///设置服务状态
    this->setAppRunValid(false);
    ///设置服务信息
    this->setAppRunFailReason("Starting..............");
	CwxDate::getDateY4MDHMS2(time(NULL), m_strStartTime);

    //启动store driver
    char szErr2K[2048];
    string strEnginePath = m_config.getCommon().m_strWorkDir + "engine/";
	if (m_store) delete m_store;
    int ret = -1;
    do{
        m_store = new UnistorStore();
        if (0 != m_store->init(UnistorApp::storeMsgPipe, this, &m_config, strEnginePath, szErr2K)){
            CWX_ERROR(("Failure to init store, err=%s", szErr2K));
            break;
        }

        ///启动网络连接与监听
        if (0 != startNetwork()) break;

        //启动checkpoint线程
        {
            m_checkpointHandler = new UnistorHandler4Checkpoint(this);
            getCommander().regHandle(SVR_TYPE_CHECKPOINT, m_checkpointHandler);
            ///创建线程
            m_checkpointThreadPool = new CwxThreadPool(THREAD_GROUP_CHECKPOINT,
                1,
                getThreadPoolMgr(),
                &getCommander());
            ///启动线程
            if ( 0 != m_checkpointThreadPool->start(NULL)){
                CWX_ERROR(("Failure to start checkpoint thread pool"));
                break;
            }
        }

        //注册kv写handler
        m_recvWriteHandler = new UnistorHandler4RecvWrite(this);
        getCommander().regHandle(SVR_TYPE_RECV_WRITE, m_recvWriteHandler);
        //注册master数据同步处理handler
        m_masterHandler = new UnistorHandler4Master(this);
        getCommander().regHandle(SVR_TYPE_MASTER, m_masterHandler);
        //启动内部分发线程
        {
            m_innerSyncChannel = new CwxAppChannel();
            m_innerSyncThreadPool = new CwxThreadPool(THREAD_GROUP_INNER_SYNC,
                1,
                getThreadPoolMgr(),
                &getCommander(),
                UnistorApp::innerSyncThreadMain,
                this);
            ///启动线程
            CwxTss** pTss = new CwxTss*[1];
            pTss[0] = new UnistorTss();
            ((UnistorTss*)pTss[0])->init(new UnistorDispatchThreadUserObj());
            if ( 0 != m_innerSyncThreadPool->start(pTss)){
                CWX_ERROR(("Failure to start inner sync thread pool"));
                break;
            }
        }

        //启动外部分发的线程
        {
            m_outerSyncChannel = new CwxAppChannel();
            m_outerSyncThreadPool = new CwxThreadPool(THREAD_GROUP_OUTER_SYNC,
                1,
                getThreadPoolMgr(),
                &getCommander(),
                UnistorApp::outerSyncThreadMain,
                this);
            ///启动线程
            CwxTss** pTss = new CwxTss*[1];
            pTss[0] = new UnistorTss();
            ((UnistorTss*)pTss[0])->init(new UnistorDispatchThreadUserObj());
            if ( 0 != m_outerSyncThreadPool->start(pTss)){
                CWX_ERROR(("Failure to start outer sync thread pool"));
                break;
            }
        }

        //创建数据写的线程池
        {
            m_writeThreadPool = new CwxThreadPool(THREAD_GROUP_WRITE,
                1,
                getThreadPoolMgr(),
                &getCommander());
            ///创建线程的tss对象
            CwxTss** pTss = new CwxTss*[1];
            pTss[0] = new UnistorTss();
            ((UnistorTss*)pTss[0])->init(NULL);
            ///启动线程
            if ( 0 != m_writeThreadPool->start(pTss)){
                CWX_ERROR(("Failure to start write thread pool"));
                break;
            }
        }

        //创建数据查询线程池
        ///创建recv的初始化
        CWX_UINT32 i=0;
        m_recvChannel = new CwxAppChannel*[m_config.getCommon().m_uiThreadNum];
        m_recvThreadPool = new CwxThreadPool*[m_config.getCommon().m_uiThreadNum];
        m_recvArgs = new pair<UnistorApp*, CWX_UINT32>[m_config.getCommon().m_uiThreadNum];
        for (i=0; i<m_config.getCommon().m_uiThreadNum; i++){
            m_recvChannel[i] = new CwxAppChannel();
            m_recvArgs[i].first = this;
            m_recvArgs[i].second = i;
            m_recvThreadPool[i] = new CwxThreadPool(THREAD_GROUP_RECV_BASE + i,
                1,
                getThreadPoolMgr(),
                &getCommander(),
                UnistorApp::recvThreadMain,
                &m_recvArgs[i]);
            ///启动线程
            CwxTss**pTss = new CwxTss*[1];
            pTss[0] = new UnistorTss();
            ((UnistorTss*)pTss[0])->init(new UnistorRecvThreadUserObj());
            if ( 0 != m_recvThreadPool[i]->start(pTss)){
                CWX_ERROR(("Failure to start kv query thread pool"));
                break;
            }
        }
        if (i != m_config.getCommon().m_uiThreadNum) break;

        //启动master消息转发线程池
        ///转发handler资源的初始化
        UnistorHandler4Trans::init(getConfig().getCommon().m_uiTranConnNum);
        {
            m_transChannel = new CwxAppChannel();
            m_transThreadPool = new CwxThreadPool(THREAD_GROUP_TRANSFER,
                1,
                getThreadPoolMgr(),
                &getCommander(),
                UnistorApp::transThreadMain,
                this);
            ///启动线程
            CwxTss** pTss = new CwxTss*[1];
            pTss[0] = new UnistorTss();
            ((UnistorTss*)pTss[0])->init(NULL);
            if ( 0 != m_transThreadPool->start(pTss)){
                CWX_ERROR(("Failure to start trans thread pool"));
                break;
            }
        }

        //启动zk线程池
        {
            m_zkThreadPool = new CwxThreadPool(THREAD_GROUP_ZK,
                1,
                getThreadPoolMgr(),
                &getCommander(),
                UnistorApp::zkThreadMain,
                this);
            ///启动线程
            if ( 0 != m_zkThreadPool->start(NULL)){
                CWX_ERROR(("Failure to start zookeeper thread pool"));
                break;
            }
        }
        //创建zk的handler
        m_zkHandler = new UnistorHandler4Zk(this);
        if (0 != m_zkHandler->init()){
            CWX_ERROR(("Failure to init zk handler"));
        }
        ret = 0;
    }while(0);
    if (0 != ret){
        this->blockSignal(SIGQUIT);
    }
    return ret;
}


///时钟函数
void UnistorApp::onTime(CwxTimeValue const& current){
    ///调用基类的onTime函数
    CwxAppFramework::onTime(current);
	static CWX_UINT32 ttCheckTimeoutTime = 0;
    static CWX_UINT32 ttTimeBase = 0;
	CWX_UINT32 uiNow = time(NULL);
    bool bClockBack = isClockBack(ttTimeBase, uiNow);

    if (bClockBack || (uiNow - ttCheckTimeoutTime >= 1)){
        ttCheckTimeoutTime = uiNow;
        ///向除了recv队列外的其他队列送入时钟信号
        CwxMsgBlock* block = NULL;
        CwxMsgBlock* msg = CwxMsgBlockAlloc::malloc(0);
        msg->event().setEvent(CwxEventInfo::TIMEOUT_CHECK);
        msg->event().setTimestamp(uiNow);
        ///往write队列送入
        if (m_writeThreadPool){
            block = CwxMsgBlockAlloc::clone(msg);
            block->event().setSvrId(SVR_TYPE_RECV_WRITE);
            m_writeThreadPool->append(block);
        }
        ///往转发队列送入
        if (m_transThreadPool){
            block = CwxMsgBlockAlloc::clone(msg);
            block->event().setSvrId(SVR_TYPE_TRANSFER);
            m_transThreadPool->append(block);
        }
        ///往checkpoint线程送入
        if (m_checkpointThreadPool &&  m_checkpointHandler && m_checkpointHandler->isNeedCheckOut(uiNow)){
            block = CwxMsgBlockAlloc::clone(msg);
            block->event().setSvrId(SVR_TYPE_CHECKPOINT);
            m_checkpointThreadPool->append(block);
        }
        ///往zk线程送入
        if (m_zkThreadPool){
            msg->event().setSvrId(SVR_TYPE_ZK);
            m_zkThreadPool->append(msg);
        }else{
            CwxMsgBlockAlloc::free(msg);
        }
    }
}

///信号处理函数
void UnistorApp::onSignal(int signum){
    switch(signum){
    case SIGQUIT: 
        ///若监控进程通知退出，则推出
        CWX_INFO(("Recv exit signal, exit right now."));
        this->stop();
        break;
    default:
        ///其他信号，全部忽略
        CWX_INFO(("Recv signal=%d, ignore it.", signum));
        break;
    }
}

///仅仅建立连接的连接建立
int UnistorApp::onConnCreated(CWX_UINT32 uiSvrId,
                          CWX_UINT32 uiHostId,
                          CWX_HANDLE handle,
                          bool& )
{
    if ((SVR_TYPE_RECV == uiSvrId)||
        (SVR_TYPE_INNER_SYNC == uiSvrId) ||
        (SVR_TYPE_OUTER_SYNC == uiSvrId))
    {
        CwxMsgBlock* msg = CwxMsgBlockAlloc::malloc(0);
        msg->event().setSvrId(uiSvrId);
        msg->event().setHostId(uiHostId);
        msg->event().setConnId(CWX_APP_INVALID_CONN_ID);
        msg->event().setIoHandle(handle);
        msg->event().setEvent(CwxEventInfo::CONN_CREATED);
        if (SVR_TYPE_RECV == uiSvrId){///数据接收连接建立，放到recv线程
            CWX_UINT32 uiIndex = handle%m_config.getCommon().m_uiThreadNum;
            if (m_recvThreadPool[uiIndex]->append(msg) <= 1) m_recvChannel[uiIndex]->notice();
        }else if (SVR_TYPE_INNER_SYNC == uiSvrId){///内部同步
            if (m_innerSyncThreadPool->append(msg) <= 1) m_innerSyncChannel->notice();
        }else if (SVR_TYPE_OUTER_SYNC == uiSvrId){///外部同步
            if (m_outerSyncThreadPool->append(msg) <= 1) m_outerSyncChannel->notice();
        }
    }else{
        CWX_ERROR(("Unknown svr-type[%u] for connection", uiSvrId));
        CWX_ASSERT(0);
    }
    return 0;

}

///接收msg的连接建立
int UnistorApp::onConnCreated(CwxAppHandler4Msg& conn,
						  bool& ,
						  bool& )
{
    if (SVR_TYPE_MONITOR == conn.getConnInfo().getSvrId()){///如果是监控的连接建立，则建立一个string的buf，用于缓存不完整的命令
		string* buf = new string();
		conn.getConnInfo().setUserData(buf);
        return 0;
    }
    CWX_ERROR(("Unknow svr-type[%u] for connect create.", conn.getConnInfo().getSvrId()));
	return 0;
}

///连接关闭
int UnistorApp::onConnClosed(CwxAppHandler4Msg& conn){
	if (SVR_TYPE_MASTER == conn.getConnInfo().getSvrId()){///如果是master的同步连接关闭
		CwxMsgBlock* pBlock = CwxMsgBlockAlloc::malloc(0);
		pBlock->event().setSvrId(conn.getConnInfo().getSvrId());
		pBlock->event().setHostId(conn.getConnInfo().getHostId());
		pBlock->event().setConnId(conn.getConnInfo().getConnId());
		///设置事件类型
		pBlock->event().setEvent(CwxEventInfo::CONN_CLOSED);
		m_writeThreadPool->append(pBlock);
	}else if (SVR_TYPE_MONITOR == conn.getConnInfo().getSvrId()){///若是监控的连接关闭，则必须释放先前所创建的string对象。
		if (conn.getConnInfo().getUserData()){
			delete (string*)conn.getConnInfo().getUserData();
			conn.getConnInfo().setUserData(NULL);
		}
	}else{
        CWX_ERROR(("Unknown svr-type[%u]'s connection is closed.", conn.getConnInfo().getSvrId()));
		CWX_ASSERT(0);
	}
	return 0;
}


///收到消息
int UnistorApp::onRecvMsg(CwxMsgBlock* msg,
						CwxAppHandler4Msg& conn,
						CwxMsgHead const& header,
						bool& )
{
    if (SVR_TYPE_MASTER == conn.getConnInfo().getSvrId()){///只有转发来的消息
        msg->event().setSvrId(conn.getConnInfo().getSvrId());
        msg->event().setHostId(conn.getConnInfo().getHostId());
        msg->event().setConnId(conn.getConnInfo().getConnId());
        msg->event().setMsgHeader(header);
        msg->event().setEvent(CwxEventInfo::RECV_MSG);
        m_writeThreadPool->append(msg);
    }else{
        CWX_ERROR(("Recv msg from Unknown svr-type[%u]'s connection.", conn.getConnInfo().getSvrId()));
        CwxMsgBlockAlloc::free(msg);
    }
    return 0;
}

///收到消息的响应函数
int UnistorApp::onRecvMsg(CwxAppHandler4Msg& conn,
						bool& ){
	if (SVR_TYPE_MONITOR == conn.getConnInfo().getSvrId()){
		char  szBuf[1024];
		ssize_t recv_size = CwxSocket::recv(conn.getHandle(),
			szBuf,
			1024);
		if (recv_size <=0 ){ //error or signal
			if ((0==recv_size) || ((errno != EWOULDBLOCK) && (errno != EINTR))){
				return -1; //error
			}else{//signal or no data
				return 0;
			}
		}
		///监控消息
		return monitorStats(szBuf, (CWX_UINT32)recv_size, conn);
	}else{
        CWX_ERROR(("Recv msg from Unknown svr-type[%u]'s connection.", conn.getConnInfo().getSvrId()));
		CWX_ASSERT(0);
	}
	return -1;
}


void UnistorApp::destroy(){
	CWX_UINT32 i=0;
    //停止zk线程，必须限于zkHandler->stop，否则会死锁。
    if (m_zkThreadPool) m_zkThreadPool->stop();
    //停止zookeeper底层线程
    if (m_zkHandler){
        m_zkHandler->stop();
    }
    //停止recv线程
    if (m_recvThreadPool){
        for (i=0; i<m_config.getCommon().m_uiThreadNum; i++){
            if (m_recvThreadPool[i]) m_recvThreadPool[i]->stop();
        }
    }
    ///停止写线程
    if (m_writeThreadPool) m_writeThreadPool->stop();
    //停止innersync线程
    if (m_innerSyncThreadPool) m_innerSyncThreadPool->stop();
    //停止outersync线程
    if (m_outerSyncThreadPool) m_outerSyncThreadPool->stop();
    //停止checkpoint线程
    if (m_checkpointThreadPool) m_checkpointThreadPool->stop();
    //停止转发线程
    if (m_transThreadPool) m_transThreadPool->stop();

    //释放handler
    UnistorHandler4Trans::destroy();
    if (m_masterHandler) delete m_masterHandler;
    m_masterHandler = NULL;

    if (m_recvWriteHandler) delete m_recvWriteHandler;
    m_recvWriteHandler = NULL;

    if (m_checkpointHandler) delete m_checkpointHandler;
    m_checkpointHandler = NULL;

    if (m_zkHandler) delete m_zkHandler;
    m_zkHandler = NULL;

    //释放线程池及channel
    if (m_recvThreadPool){
		for (i=0; i<m_config.getCommon().m_uiThreadNum; i++){
			if (m_recvThreadPool[i]) delete m_recvThreadPool[i];
		}
        delete [] m_recvThreadPool;
        m_recvThreadPool = NULL;
    }
    if (m_recvChannel){
		for (i=0; i<m_config.getCommon().m_uiThreadNum; i++){
			if (m_recvChannel[i]) delete m_recvChannel[i];
		}
		delete [] m_recvChannel;
		m_recvChannel = NULL;
    }

	if (m_innerSyncThreadPool) delete m_innerSyncThreadPool;
    m_innerSyncThreadPool = NULL;
    if (m_innerSyncChannel) delete m_innerSyncChannel;
    m_innerSyncChannel = NULL;

    //删除外部同步的线程及channel
    if (m_outerSyncThreadPool) delete m_outerSyncThreadPool;
    m_outerSyncThreadPool = NULL;
    if (m_outerSyncChannel) delete m_outerSyncChannel;
    m_outerSyncChannel = NULL;

    //删除write pool及handler
	if (m_writeThreadPool) delete m_writeThreadPool;
    m_writeThreadPool = NULL;

    //删除checkpoint线程
	if (m_checkpointThreadPool) delete m_checkpointThreadPool;
    m_checkpointThreadPool = NULL;

    //删除转发线程
    if (m_transThreadPool) delete m_transThreadPool;
    m_transThreadPool = NULL;
    if (m_transChannel) delete m_transChannel;
    m_transChannel = NULL;

    //删除zk线程
    if (m_zkThreadPool) delete m_zkThreadPool;
    m_zkThreadPool = NULL;

    if (m_store) delete m_store;
    m_store = NULL;

    if (m_recvArgs) delete [] m_recvArgs;
    m_recvArgs = NULL;

    CwxAppFramework::destroy();
}


int UnistorApp::monitorStats(char const* buf,
                             CWX_UINT32 uiDataLen,
                             CwxAppHandler4Msg& conn)
{
	string* strCmd = (string*)conn.getConnInfo().getUserData();
	strCmd->append(buf, uiDataLen);
	CwxMsgBlock* msg = NULL;
	string::size_type end = 0;
	do{
		CwxCommon::trim(*strCmd);
		end = strCmd->find('\n');
		if (string::npos == end){
			if (strCmd->length() > 10){//无效的命令
				strCmd->erase(); ///清空接受到的命令
				///回复信息
				msg = CwxMsgBlockAlloc::malloc(1024);
				strcpy(msg->wr_ptr(), "ERROR\r\n");
				msg->wr_ptr(strlen(msg->wr_ptr()));
			}else{
				return 0;
			}
		}else{
			if (memcmp(strCmd->c_str(), "stats", 5) == 0){
				strCmd->erase(); ///清空接受到的命令
				CWX_UINT32 uiLen = packMonitorInfo();
				msg = CwxMsgBlockAlloc::malloc(uiLen);
				memcpy(msg->wr_ptr(), m_szBuf, uiLen);
				msg->wr_ptr(uiLen);
			}else if(memcmp(strCmd->c_str(), "quit", 4) == 0){
				return -1;
			}else{//无效的命令
				strCmd->erase(); ///清空接受到的命令
				///回复信息
				msg = CwxMsgBlockAlloc::malloc(1024);
				strcpy(msg->wr_ptr(), "ERROR\r\n");
				msg->wr_ptr(strlen(msg->wr_ptr()));
			}
		}
	}while(0);

	msg->send_ctrl().setConnId(conn.getConnInfo().getConnId());
	msg->send_ctrl().setSvrId(UnistorApp::SVR_TYPE_MONITOR);
	msg->send_ctrl().setHostId(0);
	msg->send_ctrl().setMsgAttr(CwxMsgSendCtrl::NONE);
	if (-1 == sendMsgByConn(msg)){
		CWX_ERROR(("Failure to send monitor reply"));
		CwxMsgBlockAlloc::free(msg);
		return -1;
	}
	return 0;
}

#define UNISTOR_MONITOR_APPEND()\
	uiLen = strlen(szLine);\
	if (uiPos + uiLen > MAX_MONITOR_REPLY_SIZE - 20) break;\
	memcpy(m_szBuf + uiPos, szLine, uiLen);\
	uiPos += uiLen; \

CWX_UINT32 UnistorApp::packMonitorInfo(){
	string strValue;
	char szTmp[64];
	char szLine[4096];
	CWX_UINT32 uiLen = 0;
	CWX_UINT32 uiPos = 0;
    CWX_UINT32 i=0;
	do{
		//输出进程pid
		CwxCommon::snprintf(szLine, 4096, "STAT pid %d\r\n", getpid());
		UNISTOR_MONITOR_APPEND();
		//输出父进程pid
		CwxCommon::snprintf(szLine, 4096, "STAT ppid %d\r\n", getppid());
		UNISTOR_MONITOR_APPEND();
		//版本号
		CwxCommon::snprintf(szLine, 4096, "STAT version %s\r\n", this->getAppVersion().c_str());
		UNISTOR_MONITOR_APPEND();
		//修改时间
		CwxCommon::snprintf(szLine, 4096, "STAT modify %s\r\n", this->getLastModifyDatetime().c_str());
		UNISTOR_MONITOR_APPEND();
		//编译时间
		CwxCommon::snprintf(szLine, 4096, "STAT compile %s\r\n", this->getLastCompileDatetime().c_str());
		UNISTOR_MONITOR_APPEND();
		//启动时间
		CwxCommon::snprintf(szLine, 4096, "STAT start %s\r\n", m_strStartTime.c_str());
		UNISTOR_MONITOR_APPEND();
        //引擎类型
		CwxCommon::snprintf(szLine, 4096, "STAT store_engine %s\r\n", getConfig().getCommon().m_strStoreType.c_str());
		UNISTOR_MONITOR_APPEND();
        //引擎版本
		CwxCommon::snprintf(szLine, 4096, "STAT store_engine version %s\r\n", getStore()->getVersion());
		UNISTOR_MONITOR_APPEND();
        //引擎状态
		CwxCommon::snprintf(szLine, 4096, "STAT store_state %s\r\n",
			m_store->isValid()?"valid":"invalid");
		UNISTOR_MONITOR_APPEND();
        if (!m_store->isValid()){
            //错误信息
            CwxCommon::snprintf(szLine, 4096, "STAT store_error %s\r\n",
                m_store->getErrMsg());
            UNISTOR_MONITOR_APPEND();
        }
        //binlog的状态
		CwxCommon::snprintf(szLine, 4096, "STAT binlog_state %s\r\n",
			m_store->getBinLogMgr()->isInvalid()?"invalid":"valid");
		UNISTOR_MONITOR_APPEND();
        //binlog错误信息
        if (!m_store->getBinLogMgr()->isInvalid()){
            CwxCommon::snprintf(szLine, 4096, "STAT binlog_error %s\r\n",
                m_store->getBinLogMgr()->isInvalid()? m_store->getBinLogMgr()->getInvalidMsg():"");
            UNISTOR_MONITOR_APPEND();
        }
        //当前最小的sid
		CwxCommon::snprintf(szLine, 4096, "STAT min_sid %s\r\n",
			CwxCommon::toString(m_store->getBinLogMgr()->getMinSid(), szTmp));
		UNISTOR_MONITOR_APPEND();
        //当前最小sid的时间戳
		CwxDate::getDateY4MDHMS2(m_store->getBinLogMgr()->getMinTimestamp(), strValue);
		CwxCommon::snprintf(szLine, 4096, "STAT min_sid_time %s\r\n",
			strValue.c_str());
		UNISTOR_MONITOR_APPEND();
        //最小的binlog文件
		CwxCommon::snprintf(szLine, 4096, "STAT min_binlog_file %s\r\n",
			m_store->getBinLogMgr()->getMinFile(strValue).c_str());
		UNISTOR_MONITOR_APPEND();
        //最大的binlog的sid
		CwxCommon::snprintf(szLine, 4096, "STAT max_sid %s\r\n",
			CwxCommon::toString(m_store->getBinLogMgr()->getMaxSid(), szTmp));
		UNISTOR_MONITOR_APPEND();
        //最大binlog sid的时间戳
		CwxDate::getDateY4MDHMS2(m_store->getBinLogMgr()->getMaxTimestamp(), strValue);
		CwxCommon::snprintf(szLine, 4096, "STAT max_sid_time %s\r\n",
			strValue.c_str());
		UNISTOR_MONITOR_APPEND();
        //最大的binlog文件
		CwxCommon::snprintf(szLine, 4096, "STAT max_binlog_file %s\r\n",
			m_store->getBinLogMgr()->getMaxFile(strValue).c_str());
		UNISTOR_MONITOR_APPEND();
        //读线程的数量
        CwxCommon::snprintf(szLine, 4096, "STAT read_thread_num %u\r\n",
            m_config.getCommon().m_uiThreadNum);
        UNISTOR_MONITOR_APPEND();
        UnistorTss * tss=NULL;
        for (i=0; i<m_config.getCommon().m_uiThreadNum; i++){
            tss = (UnistorTss*)getThreadPoolMgr()->getTss(THREAD_GROUP_RECV_BASE + i, 0);
            //读线程的消息队列滞留的消息
            CwxCommon::snprintf(szLine, 4096, "STAT read_thread%d_queue %u\r\n",
                i, m_recvThreadPool[i]->getQueuedMsgNum());
            UNISTOR_MONITOR_APPEND();
            //读线程的连接数
            CwxCommon::snprintf(szLine, 4096, "STAT read_thread%d_connect %u\r\n",
                i, ((UnistorRecvThreadUserObj*)tss->getUserObj())->getConnNum());
            UNISTOR_MONITOR_APPEND();
        }
        ///写线程的滞留消息数量
        CwxCommon::snprintf(szLine, 4096, "STAT write_thread_queue %u\r\n",
            m_writeThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //转发线程滞留的消息
        CwxCommon::snprintf(szLine, 4096, "STAT trans_thread_queue %u\r\n",
            m_transThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //checkpoint线程滞留的消息
        CwxCommon::snprintf(szLine, 4096, "STAT checkpoint_thread_queue %u\r\n",
            m_checkpointThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //zk线程滞留的消息
        CwxCommon::snprintf(szLine, 4096, "STAT zk_thread_queue %u\r\n",
            m_zkThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //内部转发线程滞留的消息
        CwxCommon::snprintf(szLine, 4096, "STAT inner_sync_thread_queue %u\r\n",
            m_innerSyncThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //外部转发线程滞留的消息
        CwxCommon::snprintf(szLine, 4096, "STAT outer_sync_thread_queue %u\r\n",
            m_outerSyncThreadPool->getQueuedMsgNum());
        UNISTOR_MONITOR_APPEND();
        //zookeeper的连接状态
        CwxCommon::snprintf(szLine, 4096, "STAT zk_state %s\r\n",
            m_zkHandler->isValid()?"valid":"invalid");
        UNISTOR_MONITOR_APPEND();
        //zookeeper的错误
        if (!m_zkHandler->isValid()){
            m_zkHandler->getErrMsg(strValue);
            CwxCommon::snprintf(szLine, 4096, "STAT zk_error %s\r\n", strValue.c_str());
            UNISTOR_MONITOR_APPEND();
        }
        //cache的状态
        CwxCommon::snprintf(szLine, 4096, "STAT cache_state %s\r\n",
            m_store->getStoreEngine()->isValid()?"valid":"invalid");
        UNISTOR_MONITOR_APPEND();
        //cache的错误信息
        if (!m_store->getStoreEngine()->isValid()){
            CwxCommon::snprintf(szLine, 4096, "STAT zk_error %s\r\n", m_store->getStoreEngine()->getCache()->getErrMsg());
            UNISTOR_MONITOR_APPEND();
        }
        //cache的write key的数量
        CwxCommon::snprintf(szLine, 4096, "STAT write_cache_key %u\r\n",
            m_store->getStoreEngine()->getCache()->getWriteCacheKeyNum());
        UNISTOR_MONITOR_APPEND();
        //cache的write key的空间
        CwxCommon::snprintf(szLine, 4096, "STAT write_cache_space %u\r\n",
            m_store->getStoreEngine()->getCache()->getWriteCacheUsedSize());
        UNISTOR_MONITOR_APPEND();


        //read cache的空间
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_max_size %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->maxSize(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的最大数量
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_max_key %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getMaxCacheKeyNum(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的使用空间
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_used_size %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getUsedSize(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的使用容量
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_used_capacity %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getUsedCapacity(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的数据容量
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_used_data_size %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getUsedDataSize(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的free空间
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_free_size %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getFreeSize(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的free容量
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_free_capacity %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getFreeCapacity(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的cache的key的数量
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_key_num %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getCachedKeyCount(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的使用的item
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_used_element %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getCachedItemCount(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();
        //read cache的空闲的item
        CwxCommon::snprintf(szLine, 4096, "STAT read_cache_free_element %s\r\n",
            CwxCommon::toString((CWX_UINT64)m_store->getStoreEngine()->getCache()->getFreeItemCount(),szTmp, 10));
        UNISTOR_MONITOR_APPEND();

	}while(0);
	strcpy(m_szBuf + uiPos, "END\r\n");
	return strlen(m_szBuf);

}

///分发channel的队列消息函数。返回值：0：正常；-1：队列停止
int UnistorApp::dealRecvThreadQueue(UnistorTss* tss,
                                    CwxMsgQueue* queue,
                                    CWX_UINT32 uiQueueIndex,
                                    UnistorApp* app,
                                    CwxAppChannel*)
{
    int iRet = 0;
    CwxMsgBlock* block = NULL;
    while (!queue->isEmpty()){
        do{
            iRet = queue->dequeue(block);
            if (-1 == iRet) return -1;
            CWX_ASSERT(block->event().getSvrId() == SVR_TYPE_RECV);
            UnistorHandler4Recv::doEvent(app, tss, block, uiQueueIndex);
        } while(0);
        if (block) CwxMsgBlockAlloc::free(block);
        block = NULL;
    }
    if (queue->isDeactived()) return -1;
    return 0;
}


///分发channel的线程函数，arg为app对象
void* UnistorApp::recvThreadMain(CwxTss* tss, CwxMsgQueue* queue, void* arg)
{
	pair<UnistorApp*, CWX_UINT32>* item = (pair<UnistorApp*, CWX_UINT32>*)arg;
    if (0 != item->first->m_recvChannel[item->second]->open()){
        CWX_ERROR(("Failure to open unistor query channel, index=%d. exit......",item->second));
        ///停止进程
        item->first->stop();
        return NULL;
    }
    ((UnistorTss*)tss)->m_uiThreadIndex = item->second;
    while(1) {
        //获取队列中的消息并处理
        if (0 != dealRecvThreadQueue((UnistorTss*)tss, queue, item->second,  item->first, item->first->m_recvChannel[item->second])) break;
        if (-1 == item->first->m_recvChannel[item->second]->dispatch(1)){
            CWX_ERROR(("Failure to invoke kv query channel CwxAppChannel::dispatch(), index=%d",item->second));
            ::sleep(1);
        }
    }
    item->first->m_recvChannel[item->second]->stop();
    item->first->m_recvChannel[item->second]->close();
    if (!item->first->isStopped()) {
        CWX_INFO(("Stop app for unistor query channel thread stopped."));
        item->first->stop();
    }
    return NULL;
}

///分发channel的队列消息函数。返回值：0：正常；-1：队列停止
int UnistorApp::dealInnerSyncThreadQueue(UnistorTss* tss,
                                         CwxMsgQueue* queue,
                                         UnistorApp* app,
                                         CwxAppChannel* )
{
    int iRet = 0;
    CwxMsgBlock* block = NULL;
    while (!queue->isEmpty()){
        do {
            iRet = queue->dequeue(block);
            if (-1 == iRet) return -1;
            CWX_ASSERT(block->event().getSvrId() == SVR_TYPE_INNER_SYNC);
            UnistorHandler4Dispatch::doEvent(app, tss, block);
        } while(0);
        if (block) CwxMsgBlockAlloc::free(block);
        block = NULL;
    }
    if (queue->isDeactived()) return -1;
    return 0;
}

///分发channel的线程函数，arg为app对象
void* UnistorApp::innerSyncThreadMain(CwxTss* pThr, CwxMsgQueue* queue, void* arg){
	UnistorApp* app = (UnistorApp*) arg;
    UnistorTss* tss = (UnistorTss*)pThr;
	if (0 != app->getInnerSyncChannel()->open()){
		CWX_ERROR(("Failure to open inner sync channel, exit...."));
        app->stop();
		return NULL;
	}
	while(1){
		//获取队列中的消息并处理
		if (0 != dealInnerSyncThreadQueue(tss, queue, app, app->getInnerSyncChannel())) break;
		if (-1 == app->getInnerSyncChannel()->dispatch(1)){
			CWX_ERROR(("Failure to invoke inner sync channel CwxAppChannel::dispatch()"));
            ::sleep(1);
		}
        UnistorHandler4Dispatch::dealClosedSession(app, tss);
	}
	app->getInnerSyncChannel()->stop();
	app->getInnerSyncChannel()->close();
	if (!app->isStopped()){
		CWX_INFO(("Stop app for inner sync channel thread stopped."));
		app->stop();
	}
    ///释放线程资源
    ((UnistorDispatchThreadUserObj*)tss->getUserObj())->free(app);
	return NULL;
}

///分发channel的队列消息函数。返回值：0：正常；-1：队列停止
int UnistorApp::dealOuterSyncThreadQueue(UnistorTss* tss,
                                         CwxMsgQueue* queue,
                                         UnistorApp* app,
                                         CwxAppChannel* )
{
    int iRet = 0;
    CwxMsgBlock* block = NULL;
    while (!queue->isEmpty()){
        do {
            iRet = queue->dequeue(block);
            if (-1 == iRet) return -1;
            CWX_ASSERT(block->event().getSvrId() == SVR_TYPE_OUTER_SYNC);
            UnistorHandler4Dispatch::doEvent(app, tss, block);
        } while(0);
        CwxMsgBlockAlloc::free(block);
        block = NULL;
    }
    if (queue->isDeactived()) return -1;
    return 0;
}


///外部分发channel的线程函数，arg为app对象
void* UnistorApp::outerSyncThreadMain(CwxTss* pThr, CwxMsgQueue* queue, void* arg){
    UnistorApp* app = (UnistorApp*) arg;
    UnistorTss* tss = (UnistorTss*)pThr;
    if (0 != app->getOuterSyncChannel()->open()){
        CWX_ERROR(("Failure to open outer sync channel, exit...."));
        app->stop();
        return NULL;
    }
    while(1){
        //获取队列中的消息并处理
        if (0 != dealOuterSyncThreadQueue((UnistorTss*)tss, queue, app, app->getOuterSyncChannel())) break;
        if (-1 == app->getOuterSyncChannel()->dispatch(1)){
            CWX_ERROR(("Failure to invoke outer sync channel CwxAppChannel::dispatch()"));
            ::sleep(1);
        }
        UnistorHandler4Dispatch::dealClosedSession(app, tss);
    }
    app->getOuterSyncChannel()->stop();
    app->getOuterSyncChannel()->close();
    if (!app->isStopped()){
        CWX_INFO(("Stop app for outer sync channel thread stopped."));
        app->stop();
    }
    ///释放线程资源
    ((UnistorDispatchThreadUserObj*)tss->getUserObj())->free(app);
    return NULL;
}

///分发channel的队列消息函数。返回值：0：正常；-1：队列停止
int UnistorApp::dealTransThreadQueue(UnistorTss* tss,
                                     CwxMsgQueue* queue,
                                     UnistorApp* app,
                                     CwxAppChannel* )
{
    int iRet = 0;
    CwxMsgBlock* block = NULL;
    while (!queue->isEmpty()){
        do {
            iRet = queue->dequeue(block);
            if (-1 == iRet) return -1;
            CWX_ASSERT(block->event().getSvrId() == SVR_TYPE_TRANSFER);
            UnistorHandler4Trans::doEvent(app, tss, block);
        } while(0);
        if (block) CwxMsgBlockAlloc::free(block);
        block = NULL;
    }
    if (queue->isDeactived()) return -1;
    return 0;
}

///分发channel的线程函数，arg为app对象
void* UnistorApp::transThreadMain(CwxTss* tss, CwxMsgQueue* queue, void* arg){
    UnistorApp* app = (UnistorApp*) arg;
    if (0 != app->getTransChannel()->open()){
        CWX_ERROR(("Failure to open trans channel, exit...."));
        app->stop();
        return NULL;
    }
    while(1){
        //获取队列中的消息并处理
        if (0 != dealTransThreadQueue((UnistorTss*) tss, queue, app, app->getTransChannel())) break;
        if (-1 == app->getTransChannel()->dispatch(1)){
            CWX_ERROR(("Failure to invoke trans channel CwxAppChannel::dispatch()"));
            ::sleep(1);
        }
    }
    app->getTransChannel()->stop();
    app->getTransChannel()->close();
    if (!app->isStopped()){
        CWX_INFO(("Stop app for trans channel thread stopped."));
        app->stop();
    }

    return NULL;
}


///外部分发channel的线程函数，arg为app对象
void* UnistorApp::zkThreadMain(CwxTss* tss, CwxMsgQueue* queue, void* arg){
    UnistorApp* app = (UnistorApp*) arg;
    CwxMsgBlock* block = NULL;
    int ret = 0;
    while(-1 != (ret = queue->dequeue(block))){
        app->getZkHandler()->doEvent(tss, block, ret);
        if (block) CwxMsgBlockAlloc::free(block);
        block = NULL;
    }
    if (!app->isStopped()){
        CWX_INFO(("Stop app for zk thread is stopped."));
        app->stop();
    }
    return NULL;

}

///设置master recv连接的属性
int UnistorApp::setConnSockAttr(CWX_HANDLE handle, void* arg){
    UnistorConnAttr* attr = (UnistorConnAttr*)arg;
    if (attr->m_bNoDelay){
        int flags= 1;
        if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags)) != 0){
            CWX_ERROR(("Failure to set io TCP_NODELAY, errno=%d", errno));
        }
    }
    if (attr->m_bLinger){
        struct linger ling= {0, 0};
        if (setsockopt(handle, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling)) != 0){
            CWX_ERROR(("Failure to set io SO_LINGER, errno=%d", errno));
        }
    }
    if (attr->m_bKeepalive){
        if (0 != CwxSocket::setKeepalive(handle,
            true,
            CWX_APP_DEF_KEEPALIVE_IDLE,
            CWX_APP_DEF_KEEPALIVE_INTERNAL,
            CWX_APP_DEF_KEEPALIVE_COUNT))
        {
            CWX_ERROR(("Failure to set io Keepalive, errno=%d", errno));
        }
    }
    if (attr->m_uiSendBuf){
        int iSockBuf = (attr->m_uiSendBuf + 1023)/1024;
        iSockBuf *= 1024;
        while (setsockopt(handle, SOL_SOCKET, SO_SNDBUF, (void*)&iSockBuf, sizeof(iSockBuf)) < 0){
            iSockBuf -= 1024;
            if (iSockBuf <= 1024) break;
        }
    }
    if (attr->m_uiRecvBuf){
        int iSockBuf = (attr->m_uiRecvBuf + 1023)/1024;
        iSockBuf *= 1024;
        while(setsockopt(handle, SOL_SOCKET, SO_RCVBUF, (void *)&iSockBuf, sizeof(iSockBuf)) < 0){
            iSockBuf -= 1024;
            if (iSockBuf <= 1024) break;
        }
    }
    return 0;
}



int UnistorApp::startNetwork(){
    ///设置连接的属性
    m_recvSvrSockAttr.m_bNoDelay = m_recvCliSockAttr.m_bNoDelay = true;
    m_recvSvrSockAttr.m_bLinger = m_recvCliSockAttr.m_bLinger = true;
    m_recvSvrSockAttr.m_bKeepalive = m_recvCliSockAttr.m_bKeepalive = true;

    m_syncSvrSockAttr.m_bNoDelay = m_syncCliSockAttr.m_bNoDelay = true;
    m_syncSvrSockAttr.m_bLinger = m_syncCliSockAttr.m_bLinger = true;
    m_syncSvrSockAttr.m_bKeepalive = m_syncCliSockAttr.m_bKeepalive = true;
    m_syncSvrSockAttr.m_uiRecvBuf = m_syncCliSockAttr.m_uiSendBuf = 0;
    m_syncSvrSockAttr.m_uiSendBuf = m_syncCliSockAttr.m_uiRecvBuf = m_config.getCommon().m_uiSockBufSize * 1024;

	///启动monitor的监听
	if (m_config.getCommon().m_monitor.getHostName().length()){
		if (0 > this->noticeTcpListen(SVR_TYPE_MONITOR,
			m_config.getCommon().m_monitor.getHostName().c_str(),
			m_config.getCommon().m_monitor.getPort(),
			true)){
			CWX_ERROR(("Can't register the monitor tcp accept listen: addr=%s, port=%d",
				m_config.getCommon().m_monitor.getHostName().c_str(),
				m_config.getCommon().m_monitor.getPort()));
			return -1;
		}
	}
	///监听数据更新、查询listen
    if (0 > this->noticeTcpListen(SVR_TYPE_RECV, 
        m_config.getRecv().getHostName().c_str(),
        m_config.getRecv().getPort(),
        false,
        CWX_APP_EVENT_MODE,
        UnistorApp::setConnSockAttr,
        &m_recvSvrSockAttr))
    {
        CWX_ERROR(("Can't register the tcp accept listen: addr=%s, port=%d",
            m_config.getRecv().getHostName().c_str(),
            m_config.getRecv().getPort()));
        return -1;
    }

    ///监听内部分发
    if (0 > this->noticeTcpListen(SVR_TYPE_INNER_SYNC, 
        m_config.getInnerDispatch().getHostName().c_str(),
        m_config.getInnerDispatch().getPort(),
        false,
        CWX_APP_EVENT_MODE,
        UnistorApp::setConnSockAttr,
        &m_syncSvrSockAttr))
    {
        CWX_ERROR(("Can't register the inner-dispatch tcp accept listen: addr=%s, port=%d",
            m_config.getInnerDispatch().getHostName().c_str(),
            m_config.getInnerDispatch().getPort()));
        return -1;
    }

    ///外部监听分发
    if (0 > this->noticeTcpListen(SVR_TYPE_OUTER_SYNC, 
        m_config.getOuterDispatch().getHostName().c_str(),
        m_config.getOuterDispatch().getPort(),
        false,
        CWX_APP_EVENT_MODE,
        UnistorApp::setConnSockAttr,
        &m_syncSvrSockAttr))
    {
        CWX_ERROR(("Can't register the outer-dispatch tcp accept listen: addr=%s, port=%d",
            m_config.getOuterDispatch().getHostName().c_str(),
            m_config.getOuterDispatch().getPort()));
        return -1;
    }
	return 0;
}

///存储驱动的消息通道
int UnistorApp::storeMsgPipe(void* app,
                             CwxMsgBlock* msg,
                             bool bWriteThread,
                             char* szErr2K)
{
    UnistorApp* pApp = (UnistorApp*)app;
    if (bWriteThread){
        if (pApp->m_writeThreadPool){
            msg->event().setSvrId(SVR_TYPE_RECV_WRITE);
            if (pApp->m_writeThreadPool->append(msg) < 0){
                if (szErr2K) strcpy(szErr2K, "Failure to push store msg to write thread pool.");
                return -1;
            }
        }else{
            return -1;
        }
    }else{
        if (pApp->m_checkpointThreadPool){
            msg->event().setSvrId(SVR_TYPE_CHECKPOINT);
            if (pApp->m_checkpointThreadPool->append(msg) < 0){
                if (szErr2K) strcpy(szErr2K, "Failure to push store msg to checkpoint thread pool.");
                return -1;
            }
        }else{
            return -1;
        }
    }
    return 0;
}
