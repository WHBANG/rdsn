/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <dsn/dist/replication/duplication_common.h>
#include <dsn/dist/fmt_logging.h>
#include <dsn/utility/chrono_literals.h>
#include <dsn/utility/string_conv.h>

#include "dist/replication/meta_server/meta_service.h"
#include "meta_duplication_service.h"

namespace dsn {
namespace replication {

using namespace literals::chrono_literals;

// ThreadPool(READ): THREAD_POOL_META_SERVER
void meta_duplication_service::query_duplication_info(const duplication_query_request &request,
                                                      duplication_query_response &response)
{
    ddebug_f("query duplication info for app: {}", request.app_name);

    response.err = ERR_OK;
    {
        zauto_read_lock l(app_lock());
        std::shared_ptr<app_state> app = _state->get_app(request.app_name);
        if (!app || app->status != app_status::AS_AVAILABLE) {
            response.err = ERR_APP_NOT_EXIST;
        } else {
            response.appid = app->app_id;
            for (auto &dup_id_to_info : app->duplications) {
                const duplication_info_s_ptr &dup = dup_id_to_info.second;
                dup->to_duplication_entry_for_query(response.entry_list);
            }
        }
    }
}

// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::change_duplication_status(duplication_status_change_rpc rpc)
{
    const auto &request = rpc.request();
    auto &response = rpc.response();

    ddebug_f("change status of duplication({}) to {} for app({})",
             request.dupid,
             duplication_status_to_string(request.status),
             request.app_name);

    dupid_t dupid = request.dupid;

    std::shared_ptr<app_state> app = _state->get_app(request.app_name);
    if (!app || app->status != app_status::AS_AVAILABLE) {
        response.err = ERR_APP_NOT_EXIST;
        return;
    }

    duplication_info_s_ptr dup = app->duplications[dupid];
    if (dup == nullptr) {
        response.err = ERR_OBJECT_NOT_FOUND;
        return;
    }

    response.err = dup->alter_status(request.status);
    if (response.err != ERR_OK) {
        return;
    }

    // validation passed
    do_change_duplication_status(app, dup, rpc);
}

// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::do_change_duplication_status(std::shared_ptr<app_state> &app,
                                                            duplication_info_s_ptr &dup,
                                                            duplication_status_change_rpc &rpc)
{
    // store the duplication in requested status.
    blob value = dup->to_json_blob_in_status(rpc.request().status);

    _meta_svc->get_meta_storage()->set_data(
        std::string(dup->store_path), std::move(value), [rpc, app, dup]() {
            ddebug_f("change duplication status on storage service successfully, app name: {}, "
                     "appid: {} dupid: {}",
                     app->app_name,
                     app->app_id,
                     dup->id);

            dup->stable_status();
            rpc.response().err = ERR_OK;
            rpc.response().appid = app->app_id;
        });
}

// Note that the rpc will not create a new one if the duplication
// with the same app and remote end point already exists.
// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::add_duplication(duplication_add_rpc rpc)
{
    const auto &request = rpc.request();
    auto &response = rpc.response();
    std::shared_ptr<app_state> app;

    ddebug_f("add duplication for app({}), remote cluster address is {}",
             request.app_name,
             request.remote_cluster_address);

    response.err = ERR_OK;

    auto remote_cluster_id = get_duplication_cluster_id(request.remote_cluster_address);
    if (!remote_cluster_id.is_ok()) {
        dwarn("get_duplication_cluster_id(%s) failed: err_ret: %s",
              request.remote_cluster_address.c_str(),
              remote_cluster_id.get_error().description().c_str());
        response.err = ERR_INVALID_PARAMETERS;
        return;
    }

    app = _state->get_app(request.app_name);
    if (!app || app->status != app_status::AS_AVAILABLE) {
        response.err = ERR_APP_NOT_EXIST;
        return;
    }
    //        if (app->envs["value_version"] != "1") {
    //            dwarn("unable to add duplication for %s since value_version(%s) is not \"1\"",
    //                  request.app_name.c_str(),
    //                  app->envs["value_version"].c_str());
    //            response.err = ERR_INVALID_VERSION;
    //            return;
    //        }
    duplication_info_s_ptr dup;
    for (const auto &ent : app->duplications) {
        auto it = ent.second;
        if (it->remote == request.remote_cluster_address) {
            dup = ent.second;
            break;
        }
    }
    if (!dup) {
        dup = new_dup_from_init(request.remote_cluster_address, app.get());
    }
    do_add_duplication(app, dup, rpc);
}

// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::do_add_duplication(std::shared_ptr<app_state> &app,
                                                  duplication_info_s_ptr &dup,
                                                  duplication_add_rpc &rpc)
{
    dup->start();

    // store the duplication in started state
    blob value = dup->to_json_blob_in_status(duplication_status::DS_START);

    std::queue<std::string> nodes({get_duplication_path(*app), std::to_string(dup->id)});
    _meta_svc->get_meta_storage()->create_node_recursively(
        std::move(nodes), std::move(value), [app, dup, rpc]() mutable {
            ddebug_f("add duplication successfully, app name: {}, appid: {},"
                     " remote cluster address: {}, dupid: {}",
                     app->app_name,
                     app->app_id,
                     dup->remote,
                     dup->id);

            // The duplication starts only after it's been persisted.
            dup->stable_status();

            auto &resp = rpc.response();
            resp.err = ERR_OK;
            resp.appid = app->app_id;
            resp.dupid = dup->id;
        });
}

/// get all available apps on node `ns`
void meta_duplication_service::get_all_available_app(
    const node_state &ns, std::map<int32_t, std::shared_ptr<app_state>> &app_map) const
{
    ns.for_each_partition([this, &ns, &app_map](const gpid &pid) -> bool {
        if (ns.served_as(pid) != partition_status::PS_PRIMARY) {
            return true;
        }

        std::shared_ptr<app_state> app = _state->get_app(pid.get_app_id());
        if (!app || app->status != app_status::AS_AVAILABLE) {
            return true;
        }

        // must have duplication
        if (app->duplications.empty()) {
            return true;
        }

        if (app_map.find(app->app_id) == app_map.end()) {
            app_map.emplace(std::make_pair(pid.get_app_id(), std::move(app)));
        }
        return true;
    });
}

// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::duplication_sync(duplication_sync_rpc rpc)
{
    auto &request = rpc.request();
    auto &response = rpc.response();
    response.err = ERR_OK;

    node_state *ns = get_node_state(_state->_nodes, request.node, false);
    if (ns == nullptr) {
        dwarn_f("node({}) is not found in meta server", request.node.to_string());
        response.err = ERR_OBJECT_NOT_FOUND;
        return;
    }

    std::map<int32_t, std::shared_ptr<app_state>> app_map;
    get_all_available_app(*ns, app_map);

    for (const auto &kv : app_map) {
        int32_t app_id = kv.first;
        const auto &app = kv.second;

        for (const auto &kv2 : app->duplications) {
            dupid_t dup_id = kv2.first;
            const auto &dup = kv2.second;

            response.dup_map[app_id][dup_id] = dup->to_duplication_entry();
        }
    }

    /// update progress
    for (const auto &kv : request.confirm_list) {
        gpid gpid = kv.first;

        auto it = app_map.find(gpid.get_app_id());
        if (it == app_map.end()) {
            // app is unsync
            continue;
        }
        std::shared_ptr<app_state> &app = it->second;

        for (const duplication_confirm_entry &confirm : kv.second) {
            auto it2 = app->duplications.find(confirm.dupid);
            if (it2 == app->duplications.end()) {
                // dup is unsync
                continue;
            }

            duplication_info_s_ptr &dup = it2->second;
            do_update_partition_confirmed(
                dup, rpc, gpid.get_partition_index(), confirm.confirmed_decree);
        }
    }
}

void meta_duplication_service::do_update_partition_confirmed(duplication_info_s_ptr &dup,
                                                             duplication_sync_rpc &rpc,
                                                             int32_t partition_idx,
                                                             int64_t confirmed_decree)
{
    if (dup->alter_progress(partition_idx, confirmed_decree)) {
        std::string path = get_partition_path(dup, std::to_string(partition_idx));

        binary_writer writer;
        writer.write(confirmed_decree);

        _meta_svc->get_meta_storage()->create_node(
            std::string(path),
            writer.get_buffer(),
            [dup, rpc, partition_idx, confirmed_decree]() mutable {
                dup->stable_progress(partition_idx);

                rpc.response().dup_map[dup->app_id][dup->id].progress[partition_idx] =
                    confirmed_decree;

                // rpc will finally be replied when confirmed points
                // of all partitions are stored.
            });
    }
}

std::shared_ptr<duplication_info>
meta_duplication_service::new_dup_from_init(const std::string &remote_cluster_address,
                                            app_state *app) const
{
    duplication_info_s_ptr dup;

    // use current time to identify this duplication.
    auto dupid = static_cast<dupid_t>(dsn_now_ms() / 1000);
    {
        zauto_write_lock(app_lock());

        // hold write lock here to ensure that dupid is unique
        while (app->duplications.find(dupid) != app->duplications.end())
            dupid++;

        std::string dup_path = get_duplication_path(*app, std::to_string(dupid));
        dup = std::make_shared<duplication_info>(
            dupid, app->app_id, app->partition_count, remote_cluster_address, std::move(dup_path));

        app->duplications.emplace(dup->id, dup);
    }

    return dup;
}

// ThreadPool(WRITE): THREAD_POOL_META_STATE
void meta_duplication_service::recover_from_meta_state()
{
    ddebug_f("recovering duplication states from meta storage");

    for (auto kv : _state->_exist_apps) {
        std::shared_ptr<app_state> app = kv.second;
        if (app->status != app_status::AS_AVAILABLE) {
            return;
        }

        _meta_svc->get_meta_storage()->get_children(
            get_duplication_path(*app),
            [this, app](bool node_exists, const std::vector<std::string> &dup_id_list) {
                if (!node_exists) {
                    // if there's no duplication
                    return;
                }
                for (const std::string &raw_dup_id : dup_id_list) {
                    dupid_t dup_id;
                    dassert_f(buf2int32(raw_dup_id, dup_id),
                              "invalid path: {}",
                              get_duplication_path(*app, raw_dup_id));

                    do_restore_duplication(dup_id, app);
                }
            });
    }
}

void meta_duplication_service::do_restore_duplication(dupid_t dup_id,
                                                      std::shared_ptr<app_state> app)
{
    auto dup =
        std::make_shared<duplication_info>(dup_id,
                                           app->app_id,
                                           app->partition_count,
                                           get_duplication_path(*app, std::to_string(dup_id)));
    app->duplications[dup_id] = dup;

    // restore duplication info from json
    _meta_svc->get_meta_storage()->get_data(std::string(dup->store_path), [dup](const blob &json) {
        json::json_forwarder<duplication_info>::decode(json, *dup);
    });

    // restore progress
    _meta_svc->get_meta_storage()->get_children(
        std::string(dup->store_path),
        [dup, this](bool node_exists, const std::vector<std::string> &partition_idx_list) {
            dassert_f(node_exists, "node {} must exist on meta storage", dup->store_path);

            for (const std::string &str_pid : partition_idx_list) {
                // <app_path>/duplication/<dup_id>/<partition_index>
                std::string partition_path = get_partition_path(dup, str_pid);

                int32_t partition_idx;
                dassert_f(buf2int32(str_pid, partition_idx), "invalid path: {}", partition_path);

                _meta_svc->get_meta_storage()->get_data(
                    std::move(partition_path), [dup, partition_idx](const blob &value) {
                        int64_t confirmed_decree;
                        binary_reader reader(value);
                        reader.read(confirmed_decree);

                        dup->init_progress(partition_idx, confirmed_decree);
                    });
            }
        });
}

} // namespace replication
} // namespace dsn
