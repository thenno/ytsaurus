from .cluster_configuration import modify_cluster_configuration, NODE_MEMORY_LIMIT_ADDITION

from yt.environment import YTInstance
from yt.environment.init_cluster import initialize_world
from yt.environment.helpers import wait_for_removing_file_lock
from yt.wrapper.common import generate_uuid, GB
from yt.common import YtError, require, get_value, is_process_alive

import yt.yson as yson
import yt.json as json

from yt.packages.six.moves import map as imap, filter as ifilter

import yt.wrapper as yt

import os
import sys
import signal
import errno
import logging
import shutil
import socket
import time
import codecs
import fcntl
from functools import partial

logger = logging.getLogger("Yt.local")

def _load_config(path, is_proxy_config=False):
    if path is None:
        return {}
    with open(path, "rb") as f:
        if not is_proxy_config:
            return yson.load(f)
        else:
            return json.load(codecs.getreader("utf-8")(f))

def get_root_path(path=None):
    if path is not None:
        return path
    else:
        return os.environ.get("YT_LOCAL_ROOT_PATH", os.getcwd())

def touch(path):
    open(path, 'a').close()

def _get_bool_from_env(name, default=False):
    value = os.environ.get(name, None)
    if value is None:
        return default
    try:
        value = int(value)
    except:
        return default
    return value == 1

def _get_attributes_from_local_dir(local_path):
    meta_file_path = os.path.join(local_path, ".meta")
    if os.path.isfile(meta_file_path):
        with open(meta_file_path, "rb") as f:
            try:
                meta = yson.load(f)
            except yson.YsonError:
                logger.exception("Failed to load meta file {0}, meta will not be processed".format(meta_file_path))
                return {}
            return meta.get("attributes", {})
    return {}

def _create_map_node_from_local_dir(local_path, dest_path, client):
    attributes = _get_attributes_from_local_dir(local_path)
    client.create("map_node", dest_path, attributes=attributes, ignore_existing=True)

def _create_node_from_local_file(local_filename, dest_filename, client):
    if not os.path.isfile(local_filename + ".meta"):
        logger.warning("Found file {0} without meta info, skipping".format(file))
        return

    with open(local_filename + ".meta", "rb") as f:
        try:
            meta = yson.load(f)
        except yson.YsonError:
            logger.exception("Failed to load meta file for table {0}, skipping".format(local_filename))
            return

        if meta["type"] != "table":
            logger.warning("Found file {0} with currently unsupported type {1}" \
                           .format(file, meta["type"]))
            return

        if "format" not in meta:
            logger.warning("Found table {0} with unspecified format".format(local_filename))
            return

        with open(local_filename, "rb") as table_file:
            client.write_table(dest_filename, table_file, format=meta["format"], raw=True)

        attributes = meta.get("attributes", {})
        for key in attributes:
            client.set_attribute(dest_filename, key, attributes[key])

def _synchronize_cypress_with_local_dir(local_cypress_dir, client):
    cypress_path_prefix = "//"

    local_cypress_dir = os.path.abspath(local_cypress_dir)
    require(os.path.exists(local_cypress_dir),
            lambda: YtError("Local Cypress directory does not exist"))

    root_attributes = _get_attributes_from_local_dir(local_cypress_dir)
    for key in root_attributes:
        client.set_attribute("/", key, root_attributes[key])

    for root, dirs, files in os.walk(local_cypress_dir):
        rel_path = os.path.abspath(root)[len(local_cypress_dir)+1:]  # +1 to skip last /
        for dir in dirs:
            _create_map_node_from_local_dir(os.path.join(root, dir),
                                            os.path.join(cypress_path_prefix, rel_path, dir),
                                            client)
        for file in files:
            if file.endswith(".meta"):
                continue
            _create_node_from_local_file(os.path.join(root, file),
                                         os.path.join(cypress_path_prefix, rel_path, file),
                                         client)

def _read_pids_file(pids_file_path):
    with open(pids_file_path) as f:
        return list(imap(int, f))

def log_started_instance_info(environment, start_proxy, prepare_only):
    logger.info("Local YT {0}, id: {1}".format(
        "prepared" if prepare_only else "started",
        environment.id))
    if start_proxy:
        logger.info("Proxy address: {0}".format(environment.get_proxy_address()))

def _safe_kill(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except OSError as err:
        if err.errno == errno.EPERM:
            logger.error("Failed to kill process with pid {0}, access denied".format(pid))
        elif err.errno == errno.ESRCH:
            logger.warning("Failed to kill process with pid {0}, process not found".format(pid))
        else:
            # According to "man 2 killpg" possible error values are
            # (EINVAL, EPERM, ESRCH)
            raise

def _initialize_world(client, environment, wait_tablet_cell_initialization,
                      configure_default_tablet_cell_bundle):
    cluster_connection = environment.configs["driver"]

    proxy_address = None
    ui_address = None
    if "proxy" in environment.configs:
        proxy_address = environment.configs["proxy"]["fqdn"]
        ui_address = "http://{0}/ui/".format(proxy_address)

    initialize_world(client, proxy_address=proxy_address, ui_address=ui_address)

    tablet_cell_attributes = {
        "changelog_replication_factor": 1,
        "changelog_read_quorum": 1,
        "changelog_write_quorum": 1
    }

    if configure_default_tablet_cell_bundle:
        client.set("//sys/tablet_cell_bundles/default/@options", tablet_cell_attributes)
        tablet_cell_attributes.clear()

    tablet_cell_id = client.create("tablet_cell", attributes=tablet_cell_attributes)

    if wait_tablet_cell_initialization:
        logger.info("Waiting for tablet cells to become ready...")
        while client.get("//sys/tablet_cells/{0}/@health".format(tablet_cell_id)) != "good":
            time.sleep(0.1)
        logger.info("Tablet cells are ready")

    # Used to automatically determine local mode from python wrapper.
    client.set("//sys/@local_mode_fqdn", socket.getfqdn())
    # Cluster connection.
    client.set("//sys/@cluster_connection", cluster_connection)

_START_DEFAULTS = {
    "master_count": 1,
    "node_count": 1,
    "scheduler_count": 1,
    "jobs_memory_limit": 16 * GB,
    "jobs_cpu_limit": 1,
    "jobs_user_slot_count": 10
}

def start(master_count=None, node_count=None, scheduler_count=None, start_proxy=True,
          master_config=None, node_config=None, scheduler_config=None, proxy_config=None,
          proxy_port=None, id=None, local_cypress_dir=None, use_proxy_from_yt_source=False,
          enable_debug_logging=False, tmpfs_path=None, port_range_start=None, fqdn=None, path=None,
          prepare_only=False, jobs_memory_limit=None, jobs_cpu_limit=None, jobs_user_slot_count=None,
          wait_tablet_cell_initialization=False, set_pdeath_sig=False, watcher_config=None):

    options = {}
    for name in _START_DEFAULTS:
        options[name] = get_value(locals()[name], _START_DEFAULTS[name])

    require(options["master_count"] >= 1, lambda: YtError("Cannot start local YT instance without masters"))

    path = get_root_path(path)
    sandbox_id = id if id is not None else generate_uuid()
    require("/" not in sandbox_id, lambda: YtError('Instance id should not contain path separator "/"'))

    sandbox_path = os.path.join(path, sandbox_id)
    sandbox_tmpfs_path = os.path.join(tmpfs_path, sandbox_id) if tmpfs_path else None

    modify_configs_func = partial(
        modify_cluster_configuration,
        master_config_patch=_load_config(master_config),
        scheduler_config_patch=_load_config(scheduler_config),
        node_config_patch=_load_config(node_config),
        proxy_config_patch=_load_config(proxy_config, is_proxy_config=True))

    # Enable capturing stderrs to file
    os.environ["YT_CAPTURE_STDERR_TO_FILE"] = "1"

    environment = YTInstance(sandbox_path,
                             has_proxy=start_proxy,
                             proxy_port=proxy_port,
                             enable_debug_logging=enable_debug_logging,
                             port_range_start=port_range_start,
                             fqdn=fqdn,
                             # XXX(asaitgalin): For parallel testing purposes.
                             port_locks_path=os.environ.get("YT_LOCAL_PORT_LOCKS_PATH"),
                             preserve_working_dir=True,
                             node_memory_limit_addition=NODE_MEMORY_LIMIT_ADDITION,
                             tmpfs_path=sandbox_tmpfs_path,
                             modify_configs_func=modify_configs_func,
                             kill_child_processes=set_pdeath_sig,
                             watcher_config=watcher_config,
                             **options)

    environment.id = sandbox_id

    use_proxy_from_yt_source = use_proxy_from_yt_source or \
            _get_bool_from_env("YT_LOCAL_USE_PROXY_FROM_SOURCE")

    require(_is_stopped(sandbox_id, path),
            lambda: YtError("Instance with id {0} is already running".format(sandbox_id)))

    pids_filename = os.path.join(environment.path, "pids.txt")
    if os.path.isfile(pids_filename):
        pids = _read_pids_file(pids_filename)
        alive_pids = list(ifilter(is_process_alive, pids))
        for pid in alive_pids:
            logger.warning("Killing alive process (pid: {0}) from previously run instance".format(pid))
            _safe_kill(pid)
        os.remove(pids_filename)

    is_started_file = os.path.join(sandbox_path, "started")
    if os.path.exists(is_started_file):
        os.remove(is_started_file)

    if not prepare_only:
        environment.start(not use_proxy_from_yt_source)

        # FIXME(asaitgalin): Remove this when st/YT-3054 is done.
        if not environment._load_existing_environment:
            client = environment.create_client()

            _initialize_world(client, environment, wait_tablet_cell_initialization,
                              (environment.abi_version[0] == 19))
            if local_cypress_dir is not None:
                _synchronize_cypress_with_local_dir(local_cypress_dir, client)

    log_started_instance_info(environment, start_proxy, prepare_only)
    touch(is_started_file)

    return environment

def _is_stopped(id, path=None):
    sandbox_path = os.path.join(get_root_path(path), id)

    if not os.path.isdir(sandbox_path):
        return True

    locked_file_path = os.path.join(sandbox_path, "locked_file")
    locked_file_descriptor = open(locked_file_path, "w+")
    try:
        fcntl.lockf(locked_file_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except IOError as error:
        if error.errno == errno.EAGAIN or error.errno == errno.EACCES:
            return False
        raise
    finally:
        locked_file_descriptor.close()

    return True

def _is_exists(id, path=None):
    sandbox_path = os.path.join(get_root_path(path), id)
    return os.path.isdir(sandbox_path)

def stop(id, remove_working_dir=False, path=None):
    require(_is_exists(id, path),
            lambda: yt.YtError("Local YT with id {0} not found".format(id)))
    require(not _is_stopped(id, path),
            lambda: yt.YtError("Local YT with id {0} is already stopped".format(id)))

    pids_file_path = os.path.join(get_root_path(path), id, "pids.txt")
    for pid in _read_pids_file(pids_file_path):
        _safe_kill(pid)
    os.remove(pids_file_path)

    wait_for_removing_file_lock(os.path.join(get_root_path(path), id, "locked_file"))

    if remove_working_dir:
        delete(id, force=True, path=path)

def delete(id, force=False, path=None):
    require(_is_exists(id, path) or force,
            lambda: yt.YtError("Local YT with id {0} not found".format(id)))
    require(_is_stopped(id, path),
            lambda: yt.YtError("Local YT environment with id {0} is not stopped".format(id)))
    shutil.rmtree(os.path.join(get_root_path(path), id), ignore_errors=True)

def get_proxy(id, path=None):
    require(_is_exists(id, path), lambda: yt.YtError("Local YT with id {0} not found".format(id)))

    info_file_path = os.path.join(get_root_path(path), id, "info.yson")
    require(os.path.exists(info_file_path),
            lambda: yt.YtError("Information file for local YT with id {0} not found".format(id)))

    with open(info_file_path, "rb") as f:
        info = yson.load(f)
        if not "proxy" in info:
            raise yt.YtError("Local YT with id {0} does not have started proxy".format(id))

    return info["proxy"]["address"]

def list_instances(path=None):
    path = get_root_path(path)
    result = []
    for dir_ in os.listdir(path):
        full_path = os.path.join(path, dir_)
        if not os.path.isdir(full_path):
            logger.info("Found unknown object in instances root: %s", full_path)
            continue

        info_file = os.path.join(full_path, "info.yson")
        if not os.path.exists(info_file):
            logger.info("Path %s does not seem to contain valid local YT instance", full_path)
            continue

        stopped = _is_stopped(dir_, path)
        if stopped:
            result.append((dir_, "stopped", None))
        else:
            try:
                proxy_address = get_proxy(dir_, path)
            except yt.YtError:
                proxy_address = None
            result.append((dir_, "running", proxy_address))

    return result
