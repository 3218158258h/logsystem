#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#define FDB_LOG_TAG "FDBTEST"
#include <fdbus/fdbus.h>

#define FCP_SERVER_NAME         "org.fdbus.example.fcp"

#ifdef __WIN32__
#define FDB_PATH_SEPARATOR "\\"
#else
#define FDB_PATH_SEPARATOR "/"
#endif

#define FCP_MAX_BLOCK_SIZE      (1 << 16)

enum FcpMsgCode
{
    FCP_MSG_START_SESSION_REQ,
    FCP_MSG_STOP_SESSION_REQ,
    FCP_MSG_NEXT_BLOCK
};

enum FcpEvtCode
{
    FCP_EVT_STATUS
};

enum FcpState
{
    FCP_ST_DLE,
    FCP_ST_SESSION_OPEN,
    FCP_ST_ON_GOING,
};

enum FcpErrorCode
{
    FCP_ERR_OK = 0,
    FCP_ERR_BUSY,
    FCP_ERR_IDLE,
    FCP_ERR_TRANSFER,
    FCP_ERR_FILE_ACCESS,
    FCP_ERR_DATA,
};

class FcpStartSessionReq : public ipc::fdbus::IFdbParcelable
{
public:
    std::string mFileName;

    void serialize(ipc::fdbus::CFdbSimpleSerializer &serializer) const
    {
        serializer << mFileName;
    }
    void deserialize(ipc::fdbus::CFdbSimpleDeserializer &deserializer)
    {
        deserializer >> mFileName;
    }
};

class FcpStartSessionRep : public ipc::fdbus::IFdbParcelable
{
public:
    FcpErrorCode mErrorCode;
    int32_t mTotalSize;

    void serialize(ipc::fdbus::CFdbSimpleSerializer &serializer) const
    {
        serializer << (int32_t)mErrorCode
                   << mTotalSize;
    }
    void deserialize(ipc::fdbus::CFdbSimpleDeserializer &deserializer)
    {
        int32_t error_code;
        deserializer >> error_code
                     >> mTotalSize;
        mErrorCode = (FcpErrorCode)error_code;
    }
};

class FcpNextBlockReq : public ipc::fdbus::IFdbParcelable
{
public:
    int32_t mOffset;
    int32_t mSize;

    void serialize(ipc::fdbus::CFdbSimpleSerializer &serializer) const
    {
        serializer << mOffset
                   << mSize;
    };
    void deserialize(ipc::fdbus::CFdbSimpleDeserializer &deserializer)
    {
        deserializer >> mOffset
                     >> mSize;
    };
};

#endif
