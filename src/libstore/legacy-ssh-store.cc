#include "archive.hh"
#include "pool.hh"
#include "remote-store.hh"
#include "serve-protocol.hh"
#include "store-api.hh"
#include "worker-protocol.hh"
#include "ssh.hh"
#include "derivations.hh"

namespace nix {

static std::string uriScheme = "ssh://";

struct LegacySSHStore : public Store
{
    const Setting<int> maxConnections{this, 1, "max-connections", "maximum number of concurrent SSH connections"};
    const Setting<Path> sshKey{this, "", "ssh-key", "path to an SSH private key"};
    const Setting<bool> compress{this, false, "compress", "whether to compress the connection"};

    // Hack for getting remote build log output.
    const Setting<int> logFD{this, -1, "log-fd", "file descriptor to which SSH's stderr is connected"};

    struct Connection
    {
        std::unique_ptr<SSHMaster::Connection> sshConn;
        FdSink to;
        FdSource from;
        int remoteVersion;
    };

    std::string host;

    ref<Pool<Connection>> connections;

    SSHMaster master;

    LegacySSHStore(const string & host, const Params & params)
        : Store(params)
        , host(host)
        , connections(make_ref<Pool<Connection>>(
            std::max(1, (int) maxConnections),
            [this]() { return openConnection(); },
            [](const ref<Connection> & r) { return true; }
            ))
        , master(
            host,
            sshKey,
            // Use SSH master only if using more than 1 connection.
            connections->capacity() > 1,
            compress,
            logFD)
    {
    }

    ref<Connection> openConnection()
    {
        auto conn = make_ref<Connection>();
        conn->sshConn = master.startCommand("nix-store --serve --write");
        conn->to = FdSink(conn->sshConn->in.get());
        conn->from = FdSource(conn->sshConn->out.get());

        try {
            conn->to << SERVE_MAGIC_1 << SERVE_PROTOCOL_VERSION;
            conn->to.flush();

            unsigned int magic = readInt(conn->from);
            if (magic != SERVE_MAGIC_2)
                throw Error("protocol mismatch with ‘nix-store --serve’ on ‘%s’", host);
            conn->remoteVersion = readInt(conn->from);
            if (GET_PROTOCOL_MAJOR(conn->remoteVersion) != 0x200)
                throw Error("unsupported ‘nix-store --serve’ protocol version on ‘%s’", host);

        } catch (EndOfFile & e) {
            throw Error("cannot connect to ‘%1%’", host);
        }

        return conn;
    };

    string getUri() override
    {
        return uriScheme + host;
    }

    void queryPathInfoUncached(const Path & path,
        std::function<void(std::shared_ptr<ValidPathInfo>)> success,
        std::function<void(std::exception_ptr exc)> failure) override
    {
        sync2async<std::shared_ptr<ValidPathInfo>>(success, failure, [&]() -> std::shared_ptr<ValidPathInfo> {
            auto conn(connections->get());

            debug("querying remote host ‘%s’ for info on ‘%s’", host, path);

            conn->to << cmdQueryPathInfos << PathSet{path};
            conn->to.flush();

            auto info = std::make_shared<ValidPathInfo>();
            conn->from >> info->path;
            if (info->path.empty()) return nullptr;
            assert(path == info->path);

            PathSet references;
            conn->from >> info->deriver;
            info->references = readStorePaths<PathSet>(*this, conn->from);
            readLongLong(conn->from); // download size
            info->narSize = readLongLong(conn->from);

            auto s = readString(conn->from);
            assert(s == "");

            return info;
        });
    }

    void addToStore(const ValidPathInfo & info, const ref<std::string> & nar,
        RepairFlag repair, CheckSigsFlag checkSigs,
        std::shared_ptr<FSAccessor> accessor) override
    {
        debug("adding path ‘%s’ to remote host ‘%s’", info.path, host);

        auto conn(connections->get());

        conn->to
            << cmdImportPaths
            << 1;
        conn->to(*nar);
        conn->to
            << exportMagic
            << info.path
            << info.references
            << info.deriver
            << 0
            << 0;
        conn->to.flush();

        if (readInt(conn->from) != 1)
            throw Error("failed to add path ‘%s’ to remote host ‘%s’, info.path, host");

    }

    void narFromPath(const Path & path, Sink & sink) override
    {
        auto conn(connections->get());

        conn->to << cmdDumpStorePath << path;
        conn->to.flush();

        /* FIXME: inefficient. */
        ParseSink parseSink; /* null sink; just parse the NAR */
        TeeSource savedNAR(conn->from);
        parseDump(parseSink, savedNAR);
        sink(*savedNAR.data);
    }

    PathSet queryAllValidPaths() override { unsupported(); }

    void queryReferrers(const Path & path, PathSet & referrers) override
    { unsupported(); }

    PathSet queryDerivationOutputs(const Path & path) override
    { unsupported(); }

    StringSet queryDerivationOutputNames(const Path & path) override
    { unsupported(); }

    Path queryPathFromHashPart(const string & hashPart) override
    { unsupported(); }

    Path addToStore(const string & name, const Path & srcPath,
        bool recursive, HashType hashAlgo,
        PathFilter & filter, RepairFlag repair) override
    { unsupported(); }

    Path addTextToStore(const string & name, const string & s,
        const PathSet & references, RepairFlag repair) override
    { unsupported(); }

    BuildResult buildDerivation(const Path & drvPath, const BasicDerivation & drv,
        BuildMode buildMode) override
    {
        auto conn(connections->get());

        conn->to
            << cmdBuildDerivation
            << drvPath
            << drv
            << settings.maxSilentTime
            << settings.buildTimeout;
        if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 2)
            conn->to
                << settings.maxLogSize;
        if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 3)
            conn->to
                << settings.buildRepeat
                << settings.enforceDeterminism;

        conn->to.flush();

        BuildResult status;
        status.status = (BuildResult::Status) readInt(conn->from);
        conn->from >> status.errorMsg;

        if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 3)
            conn->from >> status.timesBuilt >> status.isNonDeterministic >> status.startTime >> status.stopTime;

        return status;
    }

    void ensurePath(const Path & path) override
    { unsupported(); }

    void addTempRoot(const Path & path) override
    { unsupported(); }

    void addIndirectRoot(const Path & path) override
    { unsupported(); }

    Roots findRoots() override
    { unsupported(); }

    void collectGarbage(const GCOptions & options, GCResults & results) override
    { unsupported(); }

    ref<FSAccessor> getFSAccessor() override
    { unsupported(); }

    void addSignatures(const Path & storePath, const StringSet & sigs) override
    { unsupported(); }

    void computeFSClosure(const PathSet & paths,
        PathSet & out, bool flipDirection = false,
        bool includeOutputs = false, bool includeDerivers = false) override
    {
        if (flipDirection || includeDerivers) {
            Store::computeFSClosure(paths, out, flipDirection, includeOutputs, includeDerivers);
            return;
        }

        auto conn(connections->get());

        conn->to
            << cmdQueryClosure
            << includeOutputs
            << paths;
        conn->to.flush();

        auto res = readStorePaths<PathSet>(*this, conn->from);

        out.insert(res.begin(), res.end());
    }

    PathSet queryValidPaths(const PathSet & paths,
        SubstituteFlag maybeSubstitute = NoSubstitute) override
    {
        auto conn(connections->get());

        conn->to
            << cmdQueryValidPaths
            << false // lock
            << maybeSubstitute
            << paths;
        conn->to.flush();

        return readStorePaths<PathSet>(*this, conn->from);
    }

    void connect() override
    {
        auto conn(connections->get());
    }
};

static RegisterStoreImplementation regStore([](
    const std::string & uri, const Store::Params & params)
    -> std::shared_ptr<Store>
{
    if (std::string(uri, 0, uriScheme.size()) != uriScheme) return 0;
    return std::make_shared<LegacySSHStore>(std::string(uri, uriScheme.size()), params);
});

}
