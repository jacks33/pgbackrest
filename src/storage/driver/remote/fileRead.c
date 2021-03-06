/***********************************************************************************************************************************
Remote Storage File Read Driver
***********************************************************************************************************************************/
#include <fcntl.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/io/read.intern.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "common/type/convert.h"
#include "storage/driver/remote/fileRead.h"
#include "storage/fileRead.intern.h"

/***********************************************************************************************************************************
Regular expressions
***********************************************************************************************************************************/
#define BLOCK_HEADER                                                "BRBLOCK"
STRING_STATIC(BLOCK_REG_EXP_STR,                                    BLOCK_HEADER "[0-9]+");

/***********************************************************************************************************************************
Command constants
***********************************************************************************************************************************/
STRING_STATIC(STORAGE_REMOTE_COMMAND_OPEN_READ_STR,                 "storageOpenRead");
STRING_STATIC(STORAGE_REMOTE_COMMAND_OPEN_READ_IGNORE_MISSING_STR,  "bIgnoreMissing");

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct StorageDriverRemoteFileRead
{
    MemContext *memContext;
    StorageDriverRemote *storage;
    StorageFileRead *interface;
    IoRead *io;
    String *name;
    bool ignoreMissing;

    ProtocolClient *client;                                         // Protocol client for requests
    size_t remaining;                                               // Bytes remaining to be read
    bool eof;                                                       // Has the file reached eof?
};

/***********************************************************************************************************************************
Local variables
***********************************************************************************************************************************/
static struct
{
    MemContext *memContext;                                         // Mem context for protocol helper
    RegExp *blockRegExp;                                            // Regular expression to check block messages
} storageDriverRemoteFileReadLocal;

/***********************************************************************************************************************************
Create a new file
***********************************************************************************************************************************/
StorageDriverRemoteFileRead *
storageDriverRemoteFileReadNew(StorageDriverRemote *storage, ProtocolClient *client, const String *name, bool ignoreMissing)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_DRIVER_REMOTE, storage);
        FUNCTION_LOG_PARAM(PROTOCOL_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING, name);
        FUNCTION_LOG_PARAM(BOOL, ignoreMissing);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(client != NULL);
    ASSERT(name != NULL);

    StorageDriverRemoteFileRead *this = NULL;

    // Create the file object
    MEM_CONTEXT_NEW_BEGIN("StorageDriverRemoteFileRead")
    {
        this = memNew(sizeof(StorageDriverRemoteFileRead));
        this->memContext = MEM_CONTEXT_NEW();
        this->storage = storage;
        this->name = strDup(name);
        this->ignoreMissing = ignoreMissing;

        this->client = client;

        // Create block regular expression if it has not been created yet
        if (storageDriverRemoteFileReadLocal.memContext == NULL)
        {
            MEM_CONTEXT_BEGIN(memContextTop())
            {
                MEM_CONTEXT_NEW_BEGIN("StorageDriverRemoteFileReadLocal")
                {
                    storageDriverRemoteFileReadLocal.memContext = memContextCurrent();
                    storageDriverRemoteFileReadLocal.blockRegExp = regExpNew(BLOCK_REG_EXP_STR);
                }
                MEM_CONTEXT_NEW_END();
            }
            MEM_CONTEXT_END();
        }

        this->interface = storageFileReadNewP(
            strNew(STORAGE_DRIVER_REMOTE_TYPE), this,
            .ignoreMissing = (StorageFileReadInterfaceIgnoreMissing)storageDriverRemoteFileReadIgnoreMissing,
            .io = (StorageFileReadInterfaceIo)storageDriverRemoteFileReadIo,
            .name = (StorageFileReadInterfaceName)storageDriverRemoteFileReadName);

        this->io = ioReadNewP(
            this, .eof = (IoReadInterfaceEof)storageDriverRemoteFileReadEof,
            .open = (IoReadInterfaceOpen)storageDriverRemoteFileReadOpen, .read = (IoReadInterfaceRead)storageDriverRemoteFileRead);
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_LOG_RETURN(STORAGE_DRIVER_REMOTE_FILE_READ, this);
}

/***********************************************************************************************************************************
Open the file
***********************************************************************************************************************************/
bool
storageDriverRemoteFileReadOpen(StorageDriverRemoteFileRead *this)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    bool result = false;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Add optional parameters
        Variant *paramOpt = varNewKv();
        kvPut(varKv(paramOpt), varNewStr(STORAGE_REMOTE_COMMAND_OPEN_READ_IGNORE_MISSING_STR), varNewBool(this->ignoreMissing));

        // Add parameters
        Variant *param = varNewVarLst(varLstNew());
        varLstAdd(varVarLst(param), varNewStr(this->name));
        varLstAdd(varVarLst(param), paramOpt);

        // Construct command
        KeyValue *command = kvPut(kvNew(), varNewStr(PROTOCOL_COMMAND_STR), varNewStr(STORAGE_REMOTE_COMMAND_OPEN_READ_STR));
        kvPut(command, varNewStr(PROTOCOL_PARAMETER_STR), param);

        result = varIntForce(varLstGet(protocolClientExecute(this->client, command, true), 0));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, result);
}

/***********************************************************************************************************************************
Get size of the next transfer block
***********************************************************************************************************************************/
static size_t
storageDriverRemoteFileReadBlockSize(const String *message)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STRING, message);
    FUNCTION_LOG_END();

    ASSERT(message != NULL);

    if (!regExpMatch(storageDriverRemoteFileReadLocal.blockRegExp, message))
        THROW_FMT(ProtocolError, "'%s' is not a valid block size message", strPtr(message));

    FUNCTION_LOG_RETURN(SIZE, (size_t)cvtZToUInt64(strPtr(message) + sizeof(BLOCK_HEADER) - 1));
}

/***********************************************************************************************************************************
Read from a file
***********************************************************************************************************************************/
size_t
storageDriverRemoteFileRead(StorageDriverRemoteFileRead *this, Buffer *buffer, bool block)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
        FUNCTION_LOG_PARAM(BUFFER, buffer);
        FUNCTION_LOG_PARAM(BOOL, block);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(buffer != NULL && !bufFull(buffer));

    size_t result = 0;

    // Read if eof has not been reached
    if (!this->eof)
    {
        do
        {
            // If no bytes remaining then read a new block
            if (this->remaining == 0)
            {
                MEM_CONTEXT_TEMP_BEGIN()
                {
                    this->remaining = storageDriverRemoteFileReadBlockSize(ioReadLine(protocolClientIoRead(this->client)));

                    if (this->remaining == 0)
                    {
                        this->eof = true;

                        // ??? Read line with filter data -- ignored for the time-being but will need to be implemented
                        ioReadLine(protocolClientIoRead(this->client));

                        // The last message sent can be ignored because it is always 1.  This is an aritifact of the protocl layer
                        // in Perl which should be removed when converted to C.
                        ioReadLine(protocolClientIoRead(this->client));
                    }
                }
                MEM_CONTEXT_TEMP_END();
            }

            // Read if not eof
            if (!this->eof)
            {
                // If the buffer can contain all remaining bytes
                if (bufRemains(buffer) >= this->remaining)
                {
                    bufLimitSet(buffer, bufUsed(buffer) + this->remaining);
                    ioRead(protocolClientIoRead(this->client), buffer);
                    bufLimitClear(buffer);
                    this->remaining = 0;
                }
                // Else read what we can
                else
                    this->remaining -= ioRead(protocolClientIoRead(this->client), buffer);
            }
        }
        while (!this->eof && !bufFull(buffer));
    }

    FUNCTION_LOG_RETURN(SIZE, result);
}

/***********************************************************************************************************************************
Has file reached EOF?
***********************************************************************************************************************************/
bool
storageDriverRemoteFileReadEof(const StorageDriverRemoteFileRead *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->eof);
}

/***********************************************************************************************************************************
Should a missing file be ignored?
***********************************************************************************************************************************/
bool
storageDriverRemoteFileReadIgnoreMissing(const StorageDriverRemoteFileRead *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->ignoreMissing);
}

/***********************************************************************************************************************************
Get the interface
***********************************************************************************************************************************/
StorageFileRead *
storageDriverRemoteFileReadInterface(const StorageDriverRemoteFileRead *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->interface);
}

/***********************************************************************************************************************************
Get the I/O interface
***********************************************************************************************************************************/
IoRead *
storageDriverRemoteFileReadIo(const StorageDriverRemoteFileRead *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->io);
}

/***********************************************************************************************************************************
File name
***********************************************************************************************************************************/
const String *
storageDriverRemoteFileReadName(const StorageDriverRemoteFileRead *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_DRIVER_REMOTE_FILE_READ, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->name);
}
