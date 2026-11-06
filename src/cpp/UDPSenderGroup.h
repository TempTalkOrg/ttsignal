


#ifndef UDPSENDERGROUP_H_INCLUDED__
#define UDPSENDERGROUP_H_INCLUDED__

#include "UDPSender.h"
#include <vector>

class UDPSenderGroup;









class UDPSenderGroup
{
    class SenderHandler : public IUDPSenderHandler
    {
    public:
        SenderHandler(UDPSenderGroup* group, UDPSender* sender)
            : group_(group), sender_(sender) {}

        void OnSendData(uint32_t nWrite, UDPSender* pSender) override;
        void OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr) override;
        void OnCheckAvailable() override;
        void OnRestart(BCRESULT result) override;
        void OnUdpClosed() override;

    private:
        UDPSenderGroup* group_;
        UDPSender*      sender_;
    };
    
    
    

    class Config
    {
    public:
        Config() : num_of_senders(1) {}

        ~Config() = default;

        uint32_t        num_of_senders;

        BCRESULT		Init(BCFObject* pConfig);

    private:
        Config(const Config& other) = delete;
        Config& operator=(const Config& other) = delete;
    };

public:
    UDPSenderGroup();
    ~UDPSenderGroup();

    BCRESULT    Create(
                    void *logger_ctx,
                    BCTaskMgr *pTaskMgr,
                    BCTimerMgr *pTimerMgr,
                    BCSocketMgr *pSockMgr,
                    BCFObject *pConfig,
                    IUDPSenderHandler *pHandler,
                    bool bindIP = false,
                    bool bindPort = false,
                    
                    
                    int64_t initialNetworkHandle = 0);
    BCRESULT    Restart(int64_t networkHandle = 0);
    BCRESULT    Connect(BCSockAddrS& refSockAddr);
    BCRESULT    StartRecv();
    BCRESULT    Send(
                    BCSockAddrS& refSockAddr,
                    LPCVOID lpData,
                    size_t nSize);
    BCRESULT    GetSockName(BCSockAddrS& refAddr);
    
    void        SetVpnPolicy(TTVpnPolicy policy);
    void        Close();
    void        Destroy(UDPSenderGroup **ppSender);

private:
    void        OnSenderSendData(UDPSender* sender, uint32_t nWrite);
    void        OnSenderRecvData(UDPSender* sender, BCBuffer* pBuffer, BCSockAddrS& refSrcAddr);
    void        OnSenderCheckAvailable(UDPSender* sender);
    void        OnSenderRestart(UDPSender* sender, BCRESULT result);
    void        OnSenderUdpClosed(UDPSender* sender);

    struct SenderEntry {
        UDPSender*     sender;
        SenderHandler* handler;
    };

    Config                      config_;
    std::vector<SenderEntry>    senders_;
    UDPSender*                  winner_     = nullptr;
    IUDPSenderHandler*          handler_    = nullptr;
    bool                        converged_  = false;
    void*                       logger_ctx_ = nullptr;
    uint32_t                    closed_count_ = 0;
};

#endif 




