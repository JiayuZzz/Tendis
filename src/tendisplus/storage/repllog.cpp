#include <type_traits>
#include <utility>
#include <memory>
#include <vector>
#include <limits>
#include "glog/logging.h"
#include "tendisplus/storage/varint.h"
#include "tendisplus/storage/record.h"
#include "tendisplus/utils/status.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

#ifndef BINLOG_V1
ReplLogKeyV2::ReplLogKeyV2()
    :_binlogId(0),
    _version("v2") {
}

ReplLogKeyV2::ReplLogKeyV2(ReplLogKeyV2&& o)
    : _binlogId(o._binlogId) {
    o._binlogId = 0;
}

ReplLogKeyV2::ReplLogKeyV2(uint64_t binlogid)
    :_binlogId(binlogid),
    _version("v2") {
}

Expected<ReplLogKeyV2> ReplLogKeyV2::decode(const RecordKey& rk) {
    auto type = rk.getRecordType();
    if (type != RecordType::RT_BINLOG) {
        return{ ErrorCodes::ERR_DECODE,
            "ReplLogKeyV2::decode:it is not a valid binlog type " + rt2Char(type) };    // NOLINT
    }

    if (rk.getChunkId() != ReplLogKeyV2::CHUNKID ||
        rk.getDbId() != ReplLogKeyV2::DBID) {
        return{ ErrorCodes::ERR_DECODE,
            "ReplLogKeyV2::decode:chunkid or dbid is invalied" };    // NOLINT
    }

    const std::string& key = rk.getPrimaryKey();
    if (key.size() != sizeof(_binlogId)) {
        return{ ErrorCodes::ERR_DECODE, "invalid keylen" };
    }
    uint64_t binlogId = int64Decode(key.c_str());

    if (rk.getSecondaryKey().size() != 0) {
        return{ ErrorCodes::ERR_DECODE, "invalid seccondkeylen" };
    }

    return ReplLogKeyV2(binlogId);
}


Expected<ReplLogKeyV2> ReplLogKeyV2::decode(const std::string& rawKey) {
    Expected<RecordKey> rk = RecordKey::decode(rawKey);
    if (!rk.ok()) {
        return rk.status();
    }
    return decode(rk.value());
}

std::string ReplLogKeyV2::encode() const {
    std::string key;
    key.resize(sizeof(_binlogId));
    int64Encode(&key[0], _binlogId);

    // NOTE(vinchen): the subkey of ReplLogKeyV2 is empty.
    RecordKey tmpRk(ReplLogKeyV2::CHUNKID,
        ReplLogKeyV2::DBID,
        RecordType::RT_BINLOG,
        std::move(key), "");
    return tmpRk.encode();
}

// std::string& ReplLogKeyV2::updateBinlogId(std::string& encodeStr,
//        uint64_t newBinlogId) {
//    INVARIANT_D(ReplLogKeyV2::decode(encodeStr).ok());
//    size_t offset = RecordKey::getHdrSize();
//    for (size_t i = 0; i < sizeof(_binlogId); i++) {
//        encodeStr[offset + i] = static_cast<char>((newBinlogId >> ((sizeof(_binlogId) - i - 1) * 8)) & 0xff);   // NOLINT
//    }
//    return encodeStr;
//}

bool ReplLogKeyV2::operator==(const ReplLogKeyV2& o) const {
    return _binlogId == o._binlogId;
}

ReplLogKeyV2& ReplLogKeyV2::operator=(const ReplLogKeyV2& o) {
    if (this == &o) {
        return *this;
    }
    _binlogId = o._binlogId;
    return *this;
}

ReplLogValueEntryV2::ReplLogValueEntryV2()
    :_op(ReplOp::REPL_OP_NONE),
    _timestamp(0),
    _key(""),
    _val("") {
}

ReplLogValueEntryV2::ReplLogValueEntryV2(ReplLogValueEntryV2&& o)
    : _op(o._op),
    _timestamp(o._timestamp),
    _key(std::move(o._key)),
    _val(std::move(o._val)) {
    o._op = ReplOp::REPL_OP_NONE;
    o._timestamp = 0;
}

ReplLogValueEntryV2::ReplLogValueEntryV2(ReplOp op, uint64_t ts,
    const std::string& key,
    const std::string& val)
    :_op(op),
    _timestamp(ts),
    _key(key),
    _val(val) {
}

ReplLogValueEntryV2::ReplLogValueEntryV2(ReplOp op, uint64_t ts,
    std::string&& key,
    std::string&& val)
    :_op(op),
    _timestamp(ts),
    _key(std::move(key)),
    _val(std::move(val)) {
}

ReplLogValueEntryV2& ReplLogValueEntryV2::operator=(ReplLogValueEntryV2&& o) {
    if (&o == this) {
        return *this;
    }

    _op = o._op;
    _timestamp = o._timestamp;
    _val = std::move(o._val);
    _key = std::move(o._key);

    o._op = ReplOp::REPL_OP_NONE;
    o._timestamp = 0;

    return *this;
}

Expected<ReplLogValueEntryV2> ReplLogValueEntryV2::decode(const char* rawVal,
                size_t maxSize, size_t& decodeSize) {
    const uint8_t *valCstr = reinterpret_cast<const uint8_t*>(rawVal);
    if (maxSize <= sizeof(_op)) {
        return{ ErrorCodes::ERR_DECODE, "invalid replvalueentry len" };
    }
    size_t offset = 0;

    // op
    uint8_t op = valCstr[0];
    offset += sizeof(uint8_t);

    // timestamp
    auto expt = varintDecodeFwd(valCstr + offset, maxSize - offset);
    if (!expt.ok()) {
        return expt.status();
    }
    offset += expt.value().second;
    uint64_t timestamp = expt.value().first;

    // key
    expt = varintDecodeFwd(valCstr + offset, maxSize - offset);
    if (!expt.ok()) {
        return expt.status();
    }
    offset += expt.value().second;

    if (maxSize - offset < expt.value().first) {
        return{ ErrorCodes::ERR_DECODE, "invalid replvalueentry len" };
    }
    auto key = std::string(reinterpret_cast<const char*>(rawVal) + offset,
                expt.value().first);
    offset += expt.value().first;

    // val
    expt = varintDecodeFwd(valCstr + offset, maxSize - offset);
    if (!expt.ok()) {
        return expt.status();
    }
    offset += expt.value().second;

    if (maxSize - offset < expt.value().first) {
        return{ ErrorCodes::ERR_DECODE, "invalid replvalueentry len" };
    }
    auto val = std::string(reinterpret_cast<const char*>(rawVal) + offset,
                    expt.value().first);
    offset += expt.value().first;

    // output
    decodeSize = offset;

    return ReplLogValueEntryV2(static_cast<ReplOp>(op), timestamp,
            std::move(key), std::move(val));
}

size_t ReplLogValueEntryV2::maxSize() const {
    return sizeof(uint8_t) + varintMaxSize(sizeof(_timestamp)) +
        varintMaxSize(sizeof(_key.size())) +
        varintMaxSize(sizeof(_val.size())) + _val.size() + _key.size();
}

size_t ReplLogValueEntryV2::encode(uint8_t* dest, size_t destSize) const {
    INVARIANT(destSize >= maxSize());

    size_t offset = 0;
    // op
    dest[offset++] = static_cast<char>(_op);

    // ts
    auto tsBytes = varintEncode(_timestamp);
    memcpy(dest + offset, tsBytes.data(), tsBytes.size());
    offset += tsBytes.size();

    // key
    auto keyBytes = varintEncode(_key.size());
    memcpy(dest + offset, keyBytes.data(), keyBytes.size());
    offset += keyBytes.size();
    memcpy(dest + offset, _key.c_str(), _key.size());
    offset += _key.size();

    // val
    auto valBytes = varintEncode(_val.size());
    memcpy(dest + offset, valBytes.data(), valBytes.size());
    offset += valBytes.size();
    memcpy(dest + offset, _val.c_str(), _val.size());
    offset += _val.size();

    return offset;
}

std::string ReplLogValueEntryV2::encode() const {
    std::string val;
    val.resize(maxSize());

    size_t offset = encode((uint8_t*)(val.c_str()), val.size());

    // resize to the exactly size
    val.resize(offset);

    return val;
}

bool ReplLogValueEntryV2::operator==(const ReplLogValueEntryV2& o) const {
    return _op == o._op &&
        _timestamp == o._timestamp &&
        _key == o._key &&
        _val == o._val;
}

ReplLogValueV2::ReplLogValueV2()
    : _chunkId(0),
    _flag(ReplFlag::REPL_GROUP_MID),
    _txnId(Transaction::TXNID_UNINITED),
    _timestamp(0),
    _versionEp(0),
    _data(nullptr),
    _dataSize(0) {
}

ReplLogValueV2::ReplLogValueV2(ReplLogValueV2&& o)
    : _chunkId(o._chunkId),
    _flag(o._flag),
    _txnId(o._txnId),
    _timestamp(o._timestamp),
    _versionEp(o._versionEp),
    _data(o._data),
    _dataSize(o._dataSize) {
    o._chunkId = 0;
    o._flag = ReplFlag::REPL_GROUP_MID;
    o._txnId = Transaction::TXNID_UNINITED;
    o._timestamp = 0;
    o._versionEp = 0;
    o._data = nullptr;
    o._dataSize = 0;
}

ReplLogValueV2::ReplLogValueV2(uint32_t chunkId, ReplFlag flag, uint64_t txnid, uint64_t timestamp, uint64_t versionEp,
        const uint8_t* data, size_t dataSize)
    :_chunkId(chunkId),
    _flag(flag),
    _txnId(txnid),
    _timestamp(timestamp),
    _versionEp(versionEp),
    _data(data),
    _dataSize(dataSize) {
}

size_t ReplLogValueV2::fixedHeaderSize() {
    return sizeof(_chunkId) + sizeof(uint16_t) + sizeof(_txnId) + sizeof(_timestamp) + sizeof(_versionEp); // NOLINT
}

std::string ReplLogValueV2::encodeHdr() const {
    std::string header;
    header.resize(fixedHeaderSize());

    size_t offset = 0;

    // CHUNKID
    auto size = int32Encode(&header[offset], _chunkId);
    offset += size;

    // FLAG
    size = int16Encode(&header[offset], static_cast<uint16_t>(_flag));
    offset += size;

    // TXNID
    size = int64Encode(&header[offset], _txnId);
    offset += size;

    // timestamp
    size = int64Encode(&header[offset], _timestamp);
    offset += size;

    // versionEP 
    size = int64Encode(&header[offset], _versionEp);
    offset += size;

    INVARIANT(offset == fixedHeaderSize());

    return header;
}

std::string ReplLogValueV2::encode(
    const std::vector<ReplLogValueEntryV2>& vec) const {
    std::string val = encodeHdr();
    size_t offset = val.size();

    size_t maxSize = offset;
    for (auto v : vec) {
        maxSize += v.maxSize();
    }

    val.resize(maxSize);

    for (auto v : vec) {
        uint8_t* desc = (uint8_t*)val.c_str() + offset;
        size_t len = v.encode(desc, maxSize - offset);
        INVARIANT(len > 0);
        offset += len;
    }

    INVARIANT(offset <= maxSize);

    val.resize(offset);

    RecordValue tmpRv(std::move(val), RecordType::RT_BINLOG, -1);

    return tmpRv.encode();
}

Expected<ReplLogValueV2> ReplLogValueV2::decode(const std::string& s) {
    auto type = RecordValue::getRecordTypeRaw(s.c_str(), s.size());
    if (type != RecordType::RT_BINLOG) {
        return{ ErrorCodes::ERR_DECODE,
           "ReplLogValueV2::decode: it is not a valid binlog type" + rt2Char(type) };   // NOLINT
    }

    auto hdrSize = RecordValue::decodeHdrSize(s);
    if (!hdrSize.ok()) {
        return hdrSize.status();
    }

     return decode(s.c_str() + hdrSize.value(), s.size() - hdrSize.value());
}
Expected<ReplLogValueV2> ReplLogValueV2::decode(const char* str, size_t size) {
    uint32_t chunkid = 0;
    uint64_t txnid = 0;
    uint64_t timestamp = 0;
    uint64_t versionEp = 0;

    if (size < fixedHeaderSize()) {
        return{ ErrorCodes::ERR_DECODE,
            "ReplLogValueV2::decode() error, too small" };
    }

    auto keyCstr = reinterpret_cast<const uint8_t*>(str);
    size_t offset = 0;
    // chunkid
    chunkid = int32Decode(str + offset);
    offset += sizeof(chunkid);

    // flag
    auto flag = static_cast<ReplFlag>(int16Decode(str + offset));
    offset += sizeof(flag);

    // txnid
    txnid = int64Decode(str + offset);
    offset += sizeof(txnid);

    // timestamp
    timestamp = int64Decode(str + offset);
    offset += sizeof(timestamp);

    // versionEp
    versionEp = int64Decode(str + offset);
    offset += sizeof(versionEp);

    INVARIANT_D(offset == fixedHeaderSize());

    return ReplLogValueV2(chunkid, flag, txnid, timestamp, versionEp, keyCstr, size);
}

bool ReplLogValueV2::isEqualHdr(const ReplLogValueV2& o) const {
    return _chunkId == o._chunkId &&
        _flag == o._flag &&
        _txnId == o._txnId &&
        _timestamp == o._timestamp &&
        _versionEp == o._versionEp;
}

// std::string& ReplLogValueV2::updateTxnId(std::string& encodeStr,
//                uint64_t newTxnId) {
//    INVARIANT_D(ReplLogValueV2::decode(encodeStr).ok());
//    auto s = RecordValue::decodeHdrSize(encodeStr);
//    INVARIANT_D(s.ok());
//
//    // header + chunkid + flag;
//    size_t offset = s.value() + sizeof(uint32_t) + sizeof(uint16_t);
//
//    for (size_t i = 0; i < sizeof(_txnId); i++) {
//        encodeStr[offset + i] = static_cast<char>((newTxnId >> ((sizeof(_txnId) - i - 1) * 8)) & 0xff);   // NOLINT
//    }
//    return encodeStr;
//
//}

ReplLogRawV2::ReplLogRawV2(const std::string& key,
                const std::string& value) :
                _key(key), _val(value) {
}

ReplLogRawV2::ReplLogRawV2(const Record& record)
    : _key(record.getRecordKey().encode()),
    _val(record.getRecordValue().encode()) {
}

ReplLogRawV2::ReplLogRawV2(std::string&& key, std::string&& value) :
            _key(std::move(key)),
            _val(std::move(value)) {
}

ReplLogRawV2::ReplLogRawV2(ReplLogRawV2&& o)
            : _key(std::move(o._key)),
            _val(std::move(o._val)) {
}

// TODO(vinchen): low performance now
uint64_t ReplLogRawV2::getBinlogId() {
    auto k = ReplLogKeyV2::decode(_key);
    INVARIANT_D(k.ok());
    if (!k.ok()) {
        return Transaction::TXNID_UNINITED;
    }
    return k.value().getBinlogId();
}

uint64_t ReplLogRawV2::getVersionEp() {
    auto v = ReplLogValueV2::decode(_val);
    INVARIANT_D(v.ok());
    if (!v.ok()) {
        return (uint64_t)-1;
    }
    return v.value().getVersionEp();
}

uint64_t ReplLogRawV2::getTimestamp() {
    auto v = ReplLogValueV2::decode(_val);
    INVARIANT_D(v.ok());
    if (!v.ok()) {
        return 0;
    }
    return v.value().getTimestamp();
}

uint64_t ReplLogRawV2::getChunkId() {
    auto v = ReplLogValueV2::decode(_val);
    INVARIANT_D(v.ok());
    if (!v.ok()) {
        return 0;
    }
    return v.value().getChunkId();
}

size_t Binlog::writeHeader(std::stringstream& ss) {
    std::string s;
    s.resize(Binlog::HEADERSIZE);
    s[0] = (uint8_t)Binlog::VERSION;

    INVARIANT_D(Binlog::HEADERSIZE == 1);

    ss << s;
    return Binlog::HEADERSIZE;
}

size_t Binlog::decodeHeader(const char* str, size_t size) {
    INVARIANT_D(str[0] == (uint8_t)Binlog::VERSION);
    if (str[0] != Binlog::VERSION) {
        return std::numeric_limits<size_t>::max();
    }

    return Binlog::HEADERSIZE;
}

size_t Binlog::writeRepllogRaw(std::stringstream& ss, const ReplLogRawV2& repllog) {
    size_t size = 0;

    size += ssAppendSizeAndString(ss, repllog.getReplLogKey());
    size += ssAppendSizeAndString(ss, repllog.getReplLogValue());

    return size;
}

ReplLogV2::ReplLogV2(ReplLogKeyV2&& key, ReplLogValueV2&& value,
    std::vector<ReplLogValueEntryV2>&& entrys)
        : _key(std::move(key)),
        _val(std::move(value)),
        _entrys(std::move(entrys)) {
}

ReplLogV2::ReplLogV2(ReplLogV2&& o)
    : _key(std::move(o._key)),
    _val(std::move(o._val)),
    _entrys(std::move(o._entrys)) {
}

Expected<ReplLogV2> ReplLogV2::decode(const std::string& key,
    const std::string& value) {
    auto k = ReplLogKeyV2::decode(key);
    if (!k.ok()) {
        return k.status();
    }

    auto v = ReplLogValueV2::decode(value);
    if (!v.ok()) {
        return v.status();
    }

    std::vector<ReplLogValueEntryV2> entrys;

    size_t offset = ReplLogValueV2::fixedHeaderSize();
    auto data = v.value().getData();
    size_t dataSize = v.value().getDataSize();
    while (offset < dataSize) {
        size_t size = 0;
        auto entry = ReplLogValueEntryV2::decode((const char*)data + offset,
            dataSize - offset, size);
        INVARIANT(entry.ok());
        if (!entry.ok()) {
            return entry.status();
        }
        offset += size;

        entrys.emplace_back(std::move(entry.value()));
    }

    if (offset != dataSize) {
        return{ ErrorCodes::ERR_DECODE, "invalid repllogvaluev2 value len" };
    }

    return ReplLogV2(std::move(k.value()), std::move(v.value()),
            std::move(entrys));
}

uint64_t ReplLogV2::getTimestamp() const {
    INVARIANT_D(_entrys.size() > 0);
    INVARIANT_D(_val.getTimestamp() == _entrys.rbegin()->getTimestamp());
    return _entrys.rbegin()->getTimestamp();
}
#endif

}  // namespace tendisplus
