///////////////////////////////////////////////////////////////////////////////
// file : UDPSenderGroup.cpp
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UDPSenderGroup.h"
#include <BC/BCLog.h>
#include "Utils.h"

///////////////////////////////////////////////////////////////////////////////
// SenderHandler
///////////////////////////////////////////////////////////////////////////////

void UDPSenderGroup::SenderHandler::OnSendData(uint32_t nWrite, UDPSender* pSender)
{
    group_->OnSenderSendData(sender_, nWrite);
}

void UDPSenderGroup::SenderHandler::OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr)
{
    group_->OnSenderRecvData(sender_, pBuffer, refSrcAddr);
}

void UDPSenderGroup::SenderHandler::OnCheckAvailable()
{
    group_->OnSenderCheckAvailable(sender_);
}

void UDPSenderGroup::SenderHandler::OnRestart(BCRESULT result)
{
    group_->OnSenderRestart(sender_, result);
}

void UDPSenderGroup::SenderHandler::OnUdpClosed()
{
    group_->OnSenderUdpClosed(sender_);
}///////////////////////////////////////////////////////////////////////////////
// class : UDPSenderGroup::Config
///////////////////////////////////////////////////////////////////////////////

BCRESULT UDPSenderGroup::Config::Init(BCFObject* pConfig)
{
    BCFVar* pVar;

    pVar = pConfig->Get("num_of_senders");
    if (IS_BCF_NUMBER(pVar))
    {
        num_of_senders = (uint32_t)GET_BCF_INT(pVar);
    }

    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// UDPSenderGroup
///////////////////////////////////////////////////////////////////////////////

UDPSenderGroup::UDPSenderGroup()
{
}

UDPSenderGroup::~UDPSenderGroup()
{
}

BCRESULT UDPSenderGroup::Create(
    void *logger_ctx,
    BCTaskMgr *pTaskMgr,
    BCTimerMgr *pTimerMgr,
    BCSocketMgr *pSockMgr,
    BCFObject *pConfig,
    IUDPSenderHandler *pHandler,
    bool bindIP,
    bool bindPort)
{
    if (!pTaskMgr || !pTimerMgr || !pSockMgr || !pConfig || !pHandler)
    {
        return BC_R_INVALIDARG;
    }
    BCRESULT result = config_.Init(pConfig);
    if (result != BC_R_SUCCESS)
    {
        return result;
    }
    handler_    = pHandler;
    logger_ctx_ = logger_ctx;
    converged_  = false;
    winner_     = nullptr;
    closed_count_ = 0;
    senders_.reserve(config_.num_of_senders);

    for (uint32_t i = 0; i < config_.num_of_senders; i++)
    {
        UDPSender*     sender  = new UDPSender();
        SenderHandler* handler = new SenderHandler(this, sender);

        BCRESULT r = sender->Create(logger_ctx, pTaskMgr, pTimerMgr,
                                    pSockMgr, pConfig, handler,
                                    bindIP, bindPort);
        if (r != BC_R_SUCCESS)
        {
            delete handler;
            delete sender;
            LogQ(logger_ctx_, _ERROR_, "UDPSenderGroup: sender %u create failed, result=%d", i, r);
            continue;
        }

        senders_.push_back({sender, handler});
    }

    if (senders_.empty())
    {
        return BC_R_FAILURE;
    }

    LogQ(logger_ctx_, _INFO_, "UDPSenderGroup: created with %u senders", (uint32_t)senders_.size());
    return BC_R_SUCCESS;
}

BCRESULT UDPSenderGroup::Restart(int64_t networkHandle)
{
    BCRESULT last = BC_R_SUCCESS;
    for (auto& entry : senders_)
    {
        BCRESULT r = entry.sender->Restart(false, networkHandle);
        if (r != BC_R_SUCCESS)
            last = r;
    }
    return last;
}

BCRESULT UDPSenderGroup::Connect(BCSockAddrS& refSockAddr)
{
    BCRESULT last = BC_R_SUCCESS;
    for (auto& entry : senders_)
    {
        BCRESULT r = entry.sender->Connect(refSockAddr);
        if (r != BC_R_SUCCESS)
            last = r;
    }
    return last;
}

BCRESULT UDPSenderGroup::StartRecv()
{
    BCRESULT last = BC_R_SUCCESS;
    for (auto& entry : senders_)
    {
        BCRESULT r = entry.sender->StartRecv();
        if (r != BC_R_SUCCESS)
            last = r;
    }
    return last;
}

BCRESULT UDPSenderGroup::Send(
    BCSockAddrS& refSockAddr,
    LPCVOID lpData,
    size_t nSize)
{
    if (converged_ && winner_)
    {
        return winner_->Send(refSockAddr, lpData, nSize);
    }

    BCRESULT last = BC_R_SUCCESS;
    for (auto& entry : senders_)
    {
        BCRESULT r = entry.sender->Send(refSockAddr, lpData, nSize);
        if (r != BC_R_SUCCESS && r != BC_R_INPROGRESS)
            last = r;
    }
    return last;
}

BCRESULT UDPSenderGroup::GetSockName(BCSockAddrS& refAddr)
{
    if (converged_ && winner_)
    {
        return winner_->GetSockName(refAddr);
    }
    if (!senders_.empty())
    {
        return senders_[0].sender->GetSockName(refAddr);
    }
    return BC_R_FAILURE;
}

void UDPSenderGroup::Close()
{
    for (auto& entry : senders_)
    {
        entry.sender->Close();
    }
}

void UDPSenderGroup::Destroy(UDPSenderGroup **ppSender)
{
    ASSERT(ppSender && *ppSender);
    ASSERT(*ppSender == this);

    for (auto& entry : senders_)
    {
        entry.sender->Destroy(&entry.sender);
        delete entry.handler;
        entry.handler = nullptr;
    }
    senders_.clear();

    delete *ppSender;
    *ppSender = NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Callback dispatchers
///////////////////////////////////////////////////////////////////////////////

void UDPSenderGroup::OnSenderSendData(UDPSender* sender, uint32_t nWrite)
{
    if (!converged_ || sender == winner_)
    {
        handler_->OnSendData(nWrite, sender);
    }
}

void UDPSenderGroup::OnSenderRecvData(
    UDPSender* sender,
    BCBuffer* pBuffer,
    BCSockAddrS& refSrcAddr)
{
    if (!converged_)
    {
        converged_ = true;
        winner_ = sender;
        BCSockAddrS addr;
        sender->GetSockName(addr);
        char addr_str[128];
        bc_sockaddr_format(&addr, addr_str, sizeof(addr_str));
        LogQ(logger_ctx_, _INFO_, "UDPSenderGroup: converged, winner selected(%s) (%u senders total)",
             addr_str, (uint32_t)senders_.size());
    }
    handler_->OnRecvData(pBuffer, refSrcAddr);
}

void UDPSenderGroup::OnSenderCheckAvailable(UDPSender* sender)
{
    if (!converged_ || sender == winner_)
    {
        handler_->OnCheckAvailable();
    }
}

void UDPSenderGroup::OnSenderRestart(UDPSender* sender, BCRESULT result)
{
    if (!converged_ || sender == winner_)
    {
        handler_->OnRestart(result);
    }
}

void UDPSenderGroup::OnSenderUdpClosed(UDPSender* sender)
{
    closed_count_++;
    if (closed_count_ >= senders_.size())
    {
        handler_->OnUdpClosed();
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file : UDPSenderGroup.cpp
///////////////////////////////////////////////////////////////////////////////
