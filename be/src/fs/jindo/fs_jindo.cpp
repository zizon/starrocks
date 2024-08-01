// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fs/jindo/fs_jindo.h"

#include <fmt/format.h>

#include <boost/compute/detail/getenv.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <shared_mutex>

#include "boost/compute/detail/lru_cache.hpp"
#include "common/config.h"
#include "common/s3_uri.h"
#include "fs/fs_util.h"
#include "gutil/strings/split.h"
#include "jindosdk/jdo_api.h"
#include "jindosdk/jdo_defines.h"
#include "jindosdk/jdo_error.h"
#include "jindosdk/jdo_file_status.h"
#include "jindosdk/jdo_list_dir_result.h"
#include "jindosdk/jdo_options.h"
#include "util/url_parser.h"

#ifndef JINDO_LOG_INFO
#define JINDO_LOG_INFO LOG(INFO)
#endif

#ifndef JINDO_LOG_WARN
#define JINDO_LOG_WARN LOG(WARNING)
#endif

#ifndef JINDO_LOG_ERROR
#define JINDO_LOG_ERROR LOG(ERROR)
#endif

namespace starrocks::jindo::internal {

class BucketHost {
public:
    BucketHost(JdoOptions_t option = nullptr, JdoStore_t store = nullptr) : _store(store), _option(option) {}

    DISALLOW_COPY(BucketHost);

    ~BucketHost() {
        if (_store != nullptr) {
            jdo_freeStore(_store);
            _store = nullptr;
        }

        if (_option != nullptr) {
            jdo_freeOptions(_option);
            _option = nullptr;
        }
    }

    Status map(const std::function<Status(const JdoHandleCtx_t, const JdoStore_t)>&& apply,
               const JdoIOContext_t io_ctx = nullptr) const {
        auto ok = map<Status>(apply, io_ctx);
        if (ok.ok()) {
            return ok.value();
        } else {
            return ok.status();
        }
    }

    template <typename out>
    StatusOr<out> map(const std::function<const out(const JdoHandleCtx_t, const JdoStore_t)>&& apply,
                      const JdoIOContext_t io_ctx = nullptr) const {
        if (_store == nullptr) {
            return Status::IOError(fmt::format("no jidno store associated"));
        }

        // create context
        auto context = io_ctx == nullptr ? jdo_createHandleCtx1(_store) : jdo_createHandleCtx2(_store, io_ctx);

        // do
        auto value = apply(context, _store);

        // check result
        auto maybe_error = jdo_getHandleCtxErrorCode(context);
        if (maybe_error != 0) {
            // error handling
            auto message =
                    fmt::format("call jindo fail, error:{} message:{}", maybe_error, jdo_getHandleCtxErrorMsg(context));
            jdo_freeHandleCtx(context);

            // special handlilng for eof
            if (maybe_error == JDO_EOF_ERROR) {
                return Status::EndOfFile(message);
            }
            return Status::IOError(message);
        } else {
            // ok
            jdo_freeHandleCtx(context);
            return value;
        }
    }

private:
    JdoStore_t _store;
    JdoOptions_t _option;
};

const Status not_implemented = Status::NotSupported("JindoFileSystem::NotImplemented");
const std::shared_ptr<BucketHost> dummy_bucket = std::make_shared<BucketHost>();
const std::string empty_string;

} // namespace starrocks::jindo::internal

namespace starrocks::jindo::io {

class JindoStream {
public:
    virtual ~JindoStream() {
        if (_io_ctx != nullptr) {
            auto status = _bucket->map<bool>([](auto ctx, auto store) { return jdo_close(ctx, nullptr); }, _io_ctx);
            if (!status.ok()) {
                JINDO_LOG_ERROR << "fail to close " << _path << " message:" << status.status().detailed_message();
            }
            jdo_freeIOContext(_io_ctx);
        }
    }

    DISALLOW_COPY(JindoStream);

protected:
    JindoStream(std::shared_ptr<internal::BucketHost>& bucket, std::string_view path, JdoIOContext_t io_context)
            : _bucket(bucket), _path(path), _io_ctx(io_context) {};

    std::shared_ptr<internal::BucketHost> _bucket;
    std::string _path;
    JdoIOContext_t _io_ctx;
};

class JindoInputStream : public starrocks::io::SeekableInputStream, JindoStream {
public:
    JindoInputStream(std::shared_ptr<internal::BucketHost>& bucket, std::string_view path, JdoIOContext_t io_context)
            : JindoStream(bucket, path, io_context) {};

    Status seek(int64_t position) override {
        return _bucket
                ->map<int64_t>([position](auto ctx, auto store) { return jdo_seek(ctx, position, nullptr) > 0; },
                               _io_ctx)
                .status();
    };

    StatusOr<int64_t> position() override {
        return _bucket->map<int64_t>([](auto ctx, auto store) { return jdo_tell(ctx, nullptr); }, _io_ctx);
    };

    StatusOr<int64_t> get_size() override {
        return _bucket->map<int64_t>(
                [this](auto ctx, auto store) {
                    auto status = jdo_getFileStatus(ctx, _path.c_str(), nullptr);
                    auto size = jdo_getFileStatusSize(status);
                    jdo_freeFileStatus(status);
                    return size;
                },
                _io_ctx);
    };

    StatusOr<int64_t> read(void* data, int64_t count) override {
        return _bucket->map<int64_t>(
                [data, count](auto ctx, auto store) { return jdo_read(ctx, static_cast<char*>(data), count, nullptr); },
                _io_ctx);
    };
};

class JindoOutputStream : public starrocks::WritableFile, JindoStream {
public:
    JindoOutputStream(std::shared_ptr<internal::BucketHost>& bucket, std::string_view path, JdoIOContext_t io_context,
                      uint64_t file_size)
            : JindoStream(bucket, path, io_context), _file_size(file_size) {};

    Status append(const Slice& data) override {
        return _bucket
                ->map<bool>(
                        [&data, this](auto ctx, auto store) {
                            update_size(jdo_write(ctx, data.get_data(), data.get_size(), nullptr));
                            return true;
                        },
                        _io_ctx)
                .status();
    };

    Status appendv(const Slice* data, size_t cnt) override {
        for (size_t i = 0; i < cnt; i++) {
            RETURN_IF_ERROR(append(data[i]));
        }
        return Status::OK();
    };

    Status pre_allocate(uint64_t size) override { return internal::not_implemented; };

    Status close() override { return sync(); };

    Status flush(FlushMode mode) override { return sync(); };

    Status sync() override {
        auto status = _bucket->map<bool>([](auto ctx, auto store) { return jdo_flush(ctx, nullptr); }, _io_ctx);
        if (!status.ok()) {
            return status.status();
        } else if (!status.value()) {
            return Status::IOError(fmt::format("sync file:{} fail", filename()));
        }

        return status.status();
    };

    uint64_t size() const override { return _file_size; };

    const std::string& filename() const override { return _path; };

private:
    void update_size(uint64_t data) {
        if (data > 0) {
            _file_size += data;
        };
    };

    uint64_t _file_size;
};

} // namespace starrocks::jindo::io

namespace starrocks::jindo {

class JindoFileSystem : public FileSystem {
public:
    Type type() const override { return JINDO; }

    JindoFileSystem(const FSOptions& options) : _fs_options(options), _cache_mutex(), _cache(10) {};

    StatusOr<std::unique_ptr<SequentialFile>> new_sequential_file(const SequentialFileOptions& opts,
                                                                  const std::string& fname) override {
        auto maybe_status = new_input_stream(fname);
        if (!maybe_status.ok()) {
            return maybe_status.status();
        }
        return std::make_unique<SequentialFile>(std::move(maybe_status.value()), fname);
    };

    StatusOr<std::unique_ptr<RandomAccessFile>> new_random_access_file(const RandomAccessFileOptions& opts,
                                                                       const std::string& fname) override {
        auto maybe_status = new_input_stream(fname);
        if (!maybe_status.ok()) {
            return maybe_status.status();
        }
        return std::make_unique<RandomAccessFile>(std::move(maybe_status.value()), fname);
    };

    StatusOr<std::unique_ptr<WritableFile>> new_writable_file(const std::string& fname) override {
        return new_writable_file(WritableFileOptions{}, fname);
    };

    StatusOr<std::unique_ptr<WritableFile>> new_writable_file(const WritableFileOptions& opts,
                                                              const std::string& path) override {
        auto exists = path_exists(path);
        int32_t flag = JDO_OPEN_FLAG_APPEND;

        switch (opts.mode) {
        case starrocks::FileSystem::OpenMode::MUST_EXIST:
            if (!exists.ok()) {
                return Status::NotFound(exists.message());
            }
            break;
        case starrocks::FileSystem::OpenMode::MUST_CREATE:
            if (exists.ok()) {
                return Status::AlreadyExist(fmt::format("can not create file:{} as it alreay exists", path));
            }
            flag |= JDO_OPEN_FLAG_CREATE;
            break;
        case starrocks::FileSystem::OpenMode::CREATE_OR_OPEN:
            if (!exists.ok()) {
                flag |= JDO_OPEN_FLAG_CREATE;
            }
            break;
        case starrocks::FileSystem::OpenMode::CREATE_OR_OPEN_WITH_TRUNCATE:
            if (exists.ok()) {
                flag = JDO_OPEN_FLAG_OVERWRITE;
            } else {
                flag |= JDO_OPEN_FLAG_CREATE;
            }
            break;
        default:
            JINDO_LOG_WARN << "unknow open mode:" << opts.mode << "using default";
            break;
        }

        auto bucket = resolve(path);
        auto maybe_io_context = bucket->map<JdoIOContext_t>(
                [&path, flag](auto ctx, auto store) { return jdo_open(ctx, path.c_str(), flag, 755, nullptr); });
        if (!maybe_io_context.ok()) {
            return maybe_io_context.status();
        }

        // grap current filesize
        auto io_ctx = maybe_io_context.value();
        auto maybe_file = bucket->map<JdoFileStatus_t>(
                [this, &path](auto ctx, auto store) { return jdo_getFileStatus(ctx, path.c_str(), nullptr); }, io_ctx);
        if (!maybe_file.ok()) {
            jdo_freeIOContext(io_ctx);
            return maybe_file.status();
        }

        auto file = maybe_file.value();
        auto size = jdo_getFileStatusSize(file);
        jdo_freeFileStatus(file);

        return std::make_unique<io::JindoOutputStream>(bucket, path, io_ctx, size);
    };

    Status path_exists(const std::string& fname) override {
        auto status = resolve(fname)->map<Status>([&fname](auto ctx, auto store) {
            return jdo_exists(ctx, fname.c_str(), nullptr) ? Status::OK() : Status::NotFound(fname);
        });
        if (!status.ok()) {
            return status.status();
        }

        return status.value();
    }

    Status get_children(const std::string& dir, std::vector<std::string>* result) override {
        std::vector<std::string> list;

        auto status = iterate_dir(dir, [&list](auto name) {
            list.emplace_back(name);
            return true;
        });

        if (result != nullptr) {
            *result = list;
        }

        return Status::OK();
    }

    Status iterate_dir(const std::string& dir, const std::function<bool(std::string_view)>& cb) override {
        return iterate_dir2(dir, [&cb](auto entry) { return cb(entry.name); });
    };

    Status iterate_dir2(const std::string& dir, const std::function<bool(DirEntry)>& cb) override {
        auto maybe_list = resolve(dir)->map<JdoListDirResult_t>(
                [&dir, &cb](auto ctx, auto store) { return jdo_listDir(ctx, dir.c_str(), false, nullptr); });
        if (!maybe_list.ok()) {
            return maybe_list.status();
        }

        auto list = maybe_list.value();
        const int64_t total = jdo_getListDirResultSize(list);
        for (int64_t i = 0; i < total; i++) {
            auto file = jdo_getListDirFileStatus(list, i);
            auto file_path = std::string_view(jdo_getFileStatusPath(file));
            if (cb(DirEntry{
                        .name = file_path.substr(dir.length() + 1, file_path.length()),
                        .mtime = jdo_getFileStatusMtime(file),
                        .size = jdo_getFileStatusSize(file),
                        .is_dir = jdo_getFileStatusType(file) == JDO_FILE_TYPE_DIRECTORY,
                })) {
                continue;
            } else {
                break;
            }
        }

        // TODO:
        // not sure what jdo_isListDirResultTruncated and jdo_getListDirResultNextMarker do,
        // maybe paging, but find no doc about how to do it
        jdo_freeListDirResult(list);

        return Status::OK();
    };

    Status delete_file(const std::string& fname) override {
        auto exists = path_exists(fname);
        if (!exists.ok()) {
            return Status::NotFound(exists.detailed_message());
        }

        auto status = resolve(fname)->map<bool>(
                [&fname](auto ctx, auto store) { return jdo_remove(ctx, fname.c_str(), false, nullptr); });
        if (!status.ok()) {
            return status.status();
        } else if (status.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to remove {}", fname));
        }
    };

    Status create_dir(const std::string& dirname) override {
        auto exists = path_exists(dirname);
        if (exists.ok()) {
            return Status::AlreadyExist(fmt::format("path {} already exsits", dirname));
        }

        return create_dir_recursive(dirname);
    };

    Status create_dir_if_missing(const std::string& dirname, bool* created = nullptr) override {
        if (created == nullptr) {
            return create_dir_recursive(dirname);
        }

        // assume io error as not exists
        auto exists = path_exists(dirname);
        auto create = create_dir_recursive(dirname);
        if (create.ok()) {
            *created = exists.ok() ? false : true;
        } else {
            *created = false;
        }

        return create;
    };

    Status create_dir_recursive(const std::string& dirname) override {
        auto status = resolve(dirname)->map<bool>(
                [&dirname](auto ctx, auto store) { return jdo_mkdir(ctx, dirname.c_str(), true, 0755, nullptr); });
        if (!status.ok()) {
            return status.status();
        } else if (status.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to create dir{} recurisive", dirname));
        }
    };

    Status delete_dir(const std::string& dirname) override {
        bool empty = true;

        auto status = iterate_dir(dirname, [&empty](auto name) {
            empty = false;
            return false;
        });
        if (!status.ok()) {
            return status;
        } else if (!empty) {
            return Status::IOError(fmt::format("dir:{} not empty", dirname));
        }

        auto removed = resolve(dirname)->map<bool>(
                [&dirname](auto ctx, auto store) { return jdo_remove(ctx, dirname.c_str(), false, nullptr); });
        if (!removed.ok()) {
            return removed.status();
        } else if (removed.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to remove empty dir {}", dirname));
        }
    };

    Status delete_dir_recursive(const std::string& dirname) override {
        auto removed = resolve(dirname)->map<bool>(
                [&dirname](auto ctx, auto store) { return jdo_remove(ctx, dirname.c_str(), true, nullptr); });
        if (!removed.ok()) {
            return removed.status();
        } else if (removed.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to remove dir {} recursive", dirname));
        }
    };

    Status sync_dir(const std::string& dirname) override { return Status::OK(); };

    StatusOr<bool> is_directory(const std::string& path) override {
        return resolve(path)->map<bool>([&path](auto ctx, auto store) {
            auto file = jdo_getFileStatus(ctx, path.c_str(), nullptr);
            bool is_dir = jdo_getFileStatusType(file) == JDO_FILE_TYPE_DIRECTORY;
            jdo_freeFileStatus(file);

            return is_dir;
        });
    };

    Status canonicalize(const std::string& path, std::string* result) override { return internal::not_implemented; };

    StatusOr<uint64_t> get_file_size(const std::string& fname) override {
        return resolve(fname)->map<uint64_t>([&fname](auto ctx, auto store) {
            auto file = jdo_getFileStatus(ctx, fname.c_str(), nullptr);
            auto size = jdo_getFileStatusSize(file);
            jdo_freeFileStatus(file);
            return size;
        });
    };

    StatusOr<uint64_t> get_file_modified_time(const std::string& fname) override {
        return resolve(fname)->map<uint64_t>([&fname](auto ctx, auto store) {
            auto file = jdo_getFileStatus(ctx, fname.c_str(), nullptr);
            auto mtime = jdo_getFileStatusMtime(file);
            jdo_freeFileStatus(file);
            return mtime;
        });
    };

    Status rename_file(const std::string& src, const std::string& target) override {
        auto status = resolve(src)->map<bool>([&src, &target](auto ctx, auto store) {
            return jdo_rename(ctx, src.c_str(), target.c_str(), nullptr);
        });
        if (!status.ok()) {
            return status.status();
        } else if (status.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to rename {} -> {}", src, target));
        }
    };

    Status link_file(const std::string& src, const std::string& target) override {
        auto status = resolve(src)->map<bool>([&src, &target](auto ctx, auto store) {
            return jdo_createSymlink(ctx, src.c_str(), target.c_str(), true, nullptr);
        });
        if (!status.ok()) {
            return status.status();
        } else if (status.value()) {
            return Status::OK();
        } else {
            return Status::IOError(fmt::format("fail to link file {} -> {}", src, target));
        }
    };

protected:
    std::shared_ptr<internal::BucketHost> resolve(const std::string& path) const {
        S3URI parser;
        if (!parser.parse(path)) {
            JINDO_LOG_ERROR << "fail to parse path:" << path << " resolve to dummy bucket";
            return internal::dummy_bucket;
        }

        auto endpoint = fmt::format("{}://{}.{}", parser.scheme(), parser.bucket(), parser.endpoint());

        // try from cache
        _cache_mutex.lock_shared();
        if (auto cached = from_cache(endpoint)) {
            _cache_mutex.unlock_shared();
            return *cached;
        }
        _cache_mutex.unlock_shared();

        _cache_mutex.lock();
        auto bucekt = [&parser, &path, this, &endpoint]() {
            if (auto cached = from_cache(endpoint)) {
                return *cached;
            }

            if (auto pathed = from_path(path)) {
                return *pathed;
            } else if (auto provided = from_provided(parser.bucket(), endpoint)) {
                return *provided;
            } else {
                return internal::dummy_bucket;
            }
        }();
        _cache_mutex.unlock();
        return bucekt;
    }

private:
    std::optional<std::shared_ptr<internal::BucketHost>> from_cache(const std::string& endpoint) const {
        auto it = _cache.get(endpoint);
        if (it) {
            return *it;
        }
        return std::nullopt;
    }

    std::optional<std::shared_ptr<internal::BucketHost>> from_path(const std::string& path) const {
        if (auto user_info = [&path]() -> std::optional<std::pair<std::string, std::string>> {
                StringValue user_info;
                if (!UrlParser::parse_url(StringValue(path), UrlParser::USERINFO, &user_info)) {
                    JINDO_LOG_WARN << "fail parse user info of " << path;
                    return std::nullopt;
                }

                return strings::Split(user_info.to_string(), ":");
            }()) {
            StringValue host;
            if (!UrlParser::parse_url(StringValue(path), UrlParser::HOST, &host)) {
                JINDO_LOG_WARN << "fail parse authority of " << path;
                return std::nullopt;
            }

            StringValue protocal;
            if (!UrlParser::parse_url(StringValue(path), UrlParser::PROTOCOL, &protocal)) {
                JINDO_LOG_WARN << "fail parse protocol of " << path;
                return std::nullopt;
            }

            auto endpoint = fmt::format("{}://{}", protocal.to_string(), host.to_string());
            if (auto bucket = create_bucket(user_info->first.c_str(), user_info->second.c_str(), endpoint,
                                            default_option())) {
                _cache.insert(endpoint, *bucket);
                JINDO_LOG_INFO << "resolve bucket for " << endpoint << " using path encoded user info";
                return bucket;
            }
        }

        JINDO_LOG_WARN << "fail to create bucekt by path:" << path;
        return std::nullopt;
    }

    std::optional<std::shared_ptr<internal::BucketHost>> from_provided(const std::string& bucket,
                                                                       const std::string& endpoint) const {
        auto option = default_option();
        auto resolve = [&option](const std::vector<std::string>&& candidates) -> const std::string& {
            for (auto& candiate : candidates) {
                auto it = option.find(candiate);
                if (it != option.end()) {
                    return it->second;
                }
            }

            return internal::empty_string;
        };
        auto& key = resolve({fmt::format("fs.oss.bucket.{}.accessKeyId", bucket), "fs.oss.accessKeyId"});
        auto& secret = resolve({fmt::format("fs.oss.bucket.{}.accessKeySecret", bucket), "fs.oss.accessKeySecret"});

        if (auto bucket = create_bucket(key, secret, endpoint, std::move(option))) {
            _cache.insert(endpoint, *bucket);

            JINDO_LOG_INFO << "resolve bucket for " << endpoint << " static configs";
            return bucket;
        }

        JINDO_LOG_WARN << "fail to create from static config:" << endpoint;
        return std::nullopt;
    }

    std::optional<std::shared_ptr<internal::BucketHost>> create_bucket(const std::string& key,
                                                                       const std::string& secret,
                                                                       const std::string& endpoint,
                                                                       std::map<std::string, std::string>&& kv) const {
        kv.emplace("fs.oss.accessKeyId", key);
        kv.emplace("fs.oss.accessKeySecret", secret);
        kv.emplace("fs.oss.endpoint", endpoint);
        auto& user = [&kv]() -> const std::string& {
            auto it = kv.find("jindo.user");
            if (it == kv.end()) {
                return config::jindo_user;
            } else {
                return it->second;
            }
        }();

        auto option = jdo_createOptions();
        for (auto& [key, value] : kv) {
            jdo_setOption(option, key.c_str(), value.c_str());
        }

        // creaet store
        auto store = jdo_createStore(option, endpoint.c_str());
        auto ctx = jdo_createHandleCtx1(store);
        auto error = jdo_getHandleCtxErrorCode(ctx);
        jdo_init(ctx, user.c_str());
        if (error != 0) {
            JINDO_LOG_WARN << "fail to create store:" << endpoint << " reason:" << jdo_getHandleCtxErrorMsg(ctx);
            jdo_freeHandleCtx(ctx);
            jdo_destroyStore(store);
            jdo_freeStore(store);
            jdo_freeOptions(option);
            return std::nullopt;
        }
        jdo_freeHandleCtx(ctx);

        return std::make_shared<internal::BucketHost>(option, store);
    }

    std::map<std::string, std::string> default_option() const {
        std::map<std::string, std::string> option;

        auto load_from_core_site = [&option](const std::string& path) {
            try {
                boost::property_tree::ptree xml;
                boost::property_tree::read_xml(path, xml, boost::property_tree::xml_parser::trim_whitespace);

                if (auto maybe_properties = xml.get_child_optional("configuration")) {
                    for (auto [name, property] : *maybe_properties) {
                        // fail to use property.get("name") to work,
                        // a walk around using pari
                        std::pair<std::string, std::string> kv;
                        for (auto [key, value] : property) {
                            if (key == "name") {
                                kv.first = value.data();
                            } else if (key == "value") {
                                kv.second = value.data();
                            }
                        }

                        option.emplace(kv);
                    }
                }
            } catch (boost::property_tree::xml_parser_error& error) {
                JINDO_LOG_WARN << "fail to parse xml file:" << path << " reason:" << error.what();
            }
        };

        // from core-site.xml
        auto home_conf = fmt::format("{}/conf/core-site.xml", boost::compute::detail::getenv("STARROCKS_HOME"));
        if (fs::path_exist(home_conf)) {
            JINDO_LOG_INFO << "using static config:" << home_conf;
            load_from_core_site(home_conf);
        } else {
            // try from hadoop_confi_dir
            for (auto candiate : strings::Split(boost::compute::detail::getenv("HADOOP_CONF_DIR"), ":")) {
                auto conf = fmt::format("{}/core-site.xml", candiate.as_string());
                if (fs::path_exist(conf)) {
                    JINDO_LOG_INFO << "using static config:" << conf;
                    load_from_core_site(conf);
                    break;
                }
            }
        }

        return option;
    }

    StatusOr<std::unique_ptr<io::JindoInputStream>> new_input_stream(const std::string& path) const {
        auto bucket = resolve(path);
        auto maybe_io_context = bucket->map<JdoIOContext_t>([&path](auto ctx, auto store) {
            return jdo_open(ctx, path.c_str(), JDO_OPEN_FLAG_READ_ONLY, 755, nullptr);
        });
        if (!maybe_io_context.ok()) {
            return maybe_io_context.status();
        }

        return std::make_unique<io::JindoInputStream>(bucket, path, maybe_io_context.value());
    };

    const FSOptions _fs_options;
    mutable std::shared_mutex _cache_mutex;
    mutable boost::compute::detail::lru_cache<std::string, std::shared_ptr<internal::BucketHost>> _cache;
};
} // namespace starrocks::jindo

namespace starrocks {
std::unique_ptr<FileSystem> new_fs_jindo(const FSOptions& options) {
    return std::make_unique<jindo::JindoFileSystem>(options);
};
} // namespace starrocks
