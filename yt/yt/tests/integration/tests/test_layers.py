from yt_env_setup import YTEnvSetup, Restarter, NODES_SERVICE

from yt_commands import (  # noqa
    authors, print_debug, wait, retry, wait_assert, wait_breakpoint, release_breakpoint, with_breakpoint,
    events_on_fs, reset_events_on_fs,
    create, ls, get, set, copy, move, remove, link, exists, concatenate,
    create_account, create_network_project, create_tmpdir, create_user, create_group, create_medium,
    create_pool, create_pool_tree, remove_pool_tree,
    create_data_center, create_rack, create_table,
    create_tablet_cell_bundle, remove_tablet_cell_bundle, create_tablet_cell, create_table_replica,
    make_ace, check_permission, add_member, remove_member, remove_group, remove_user,
    remove_network_project,
    make_batch_request, execute_batch, get_batch_error,
    start_transaction, abort_transaction, commit_transaction, lock,
    insert_rows, select_rows, lookup_rows, delete_rows, trim_rows, alter_table,
    read_file, write_file, read_table, write_table, write_local_file, read_blob_table,
    read_journal, write_journal, truncate_journal, wait_until_sealed,
    map, reduce, map_reduce, join_reduce, merge, vanilla, sort, erase, remote_copy,
    run_test_vanilla, run_sleeping_vanilla,
    abort_job, list_jobs, get_job, abandon_job, interrupt_job,
    get_job_fail_context, get_job_input, get_job_stderr, get_job_spec, get_job_input_paths,
    dump_job_context, poll_job_shell,
    abort_op, complete_op, suspend_op, resume_op,
    get_operation, list_operations, clean_operations,
    get_operation_cypress_path, scheduler_orchid_pool_path,
    scheduler_orchid_default_pool_tree_path, scheduler_orchid_operation_path,
    scheduler_orchid_default_pool_tree_config_path, scheduler_orchid_path,
    scheduler_orchid_node_path, scheduler_orchid_pool_tree_config_path, scheduler_orchid_pool_tree_path,
    mount_table, unmount_table, freeze_table, unfreeze_table, reshard_table, remount_table, generate_timestamp,
    reshard_table_automatic, wait_for_tablet_state, wait_for_cells, wait_for_nodes,
    get_tablet_infos, get_table_pivot_keys, get_tablet_leader_address,
    sync_create_cells, sync_mount_table, sync_unmount_table,
    sync_freeze_table, sync_unfreeze_table, sync_reshard_table,
    sync_flush_table, sync_compact_table, sync_remove_tablet_cells,
    sync_reshard_table_automatic, sync_balance_tablet_cells,
    get_first_chunk_id, get_singular_chunk_id, get_chunk_replication_factor, multicell_sleep,
    update_nodes_dynamic_config, update_controller_agent_config,
    update_op_parameters, enable_op_detailed_logs,
    set_node_banned, set_banned_flag,
    set_account_disk_space_limit, set_node_decommissioned,
    get_account_disk_space, get_account_committed_disk_space,
    check_all_stderrs,
    create_test_tables, create_dynamic_table, PrepareTables,
    get_statistics, get_recursive_disk_space, get_chunk_owner_disk_space,
    make_random_string, raises_yt_error,
    build_snapshot, gc_collect, is_multicell,
    get_driver, execute_command,
    AsyncLastCommittedTimestamp)

from yt.common import YtError

import pytest

import os
import time
from functools import partial
from collections import Counter


class TestLayers(YTEnvSetup):
    NUM_SCHEDULERS = 1
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        }
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1")
        file_name = "layers/static-bin.tar.gz"
        write_file("//tmp/layer1", open(file_name).read())

        create("file", "//tmp/layer2")
        file_name = "layers/test.tar.gz"
        write_file("//tmp/layer2", open(file_name).read())

        create("file", "//tmp/corrupted_layer")
        file_name = "layers/corrupted.tar.gz"
        write_file("//tmp/corrupted_layer", open(file_name).read())

        create("file", "//tmp/static_cat")
        file_name = "layers/static_cat"
        write_file("//tmp/static_cat", open(file_name).read())

        set("//tmp/static_cat/@executable", True)

    @authors("ilpauzner")
    def test_disabled_layer_locations(self):
        with Restarter(self.Env, NODES_SERVICE):
            disabled_path = None
            for node in self.Env.configs["node"][:1]:
                for layer_location in node["data_node"]["volume_manager"]["layer_locations"]:
                    try:
                        disabled_path = layer_location["path"]
                        os.mkdir(layer_location["path"])
                    except OSError:
                        pass
                    open(layer_location["path"] + "/disabled", "w")

        wait_for_nodes()

        with Restarter(self.Env, NODES_SERVICE):
            os.unlink(disabled_path + "/disabled")
        wait_for_nodes()

        time.sleep(5)

    @authors("prime")
    def test_corrupted_layer(self):
        self.setup_files()
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        with pytest.raises(YtError):
            map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="./static_cat; ls $YT_ROOT_FS 1>&2",
                file="//tmp/static_cat",
                spec={
                    "max_failed_job_count": 1,
                    "mapper": {
                        "layer_paths": ["//tmp/layer1", "//tmp/corrupted_layer"],
                    },
                },
            )

        # YT-14186: Corrupted user layer should not disable jobs on node.
        for node in ls("//sys/cluster_nodes"):
            assert len(get("//sys/cluster_nodes/{}/@alerts".format(node))) == 0

    @authors("psushin")
    def test_one_layer(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        for job_id in ls(jobs_path):
            stderr_path = "{0}/{1}/stderr".format(jobs_path, job_id)
            assert "static-bin" in read_file(stderr_path)

    @authors("psushin")
    def test_two_layers(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1", "//tmp/layer2"],
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        for job_id in ls(jobs_path):
            stderr_path = "{0}/{1}/stderr".format(jobs_path, job_id)
            stderr = read_file(stderr_path)
            assert "static-bin" in stderr
            assert "test" in stderr

    @authors("psushin")
    def test_bad_layer(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        with pytest.raises(YtError):
            map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="./static_cat; ls $YT_ROOT_FS 1>&2",
                file="//tmp/static_cat",
                spec={
                    "max_failed_job_count": 1,
                    "mapper": {
                        "layer_paths": ["//tmp/layer1", "//tmp/bad_layer"],
                    },
                },
            )


@authors("psushin")
class TestTmpfsLayerCache(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
        "data_node": {
            "volume_manager": {
                "tmpfs_layer_cache": {
                    "capacity": 10 * 1024 * 1024,
                    "layers_directory_path": "//tmp/cached_layers",
                    "layers_update_period": 100,
                }
            }
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file("//tmp/layer1", open(file_name).read())

        create("file", "//tmp/static_cat", attributes={"replication_factor": 1})
        file_name = "layers/static_cat"
        write_file("//tmp/static_cat", open(file_name).read())

        set("//tmp/static_cat/@executable", True)

    def test_tmpfs_layer_cache(self):
        self.setup_files()

        orchid_path = "orchid/job_controller/slot_manager/root_volume_manager"

        for node in ls("//sys/cluster_nodes"):
            assert get("//sys/cluster_nodes/{0}/{1}/tmpfs_cache/layer_count".format(node, orchid_path)) == 0

        create("map_node", "//tmp/cached_layers")
        link("//tmp/layer1", "//tmp/cached_layers/layer1")

        for node in ls("//sys/cluster_nodes"):
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/tmpfs_cache/layer_count".format(node, orchid_path)) == 1)

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        for job_id in ls(jobs_path):
            stderr_path = "{0}/{1}/stderr".format(jobs_path, job_id)
            assert "static-bin" in read_file(stderr_path)

        remove("//tmp/cached_layers/layer1")
        for node in ls("//sys/cluster_nodes"):
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/tmpfs_cache/layer_count".format(node, orchid_path)) == 0)


@authors("mrkastep")
class TestJobSetup(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                }
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/layer1",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat(self):
        self.setup_files()

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                    "job_count": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"


@authors("mrkastep")
class TestGpuJobSetup(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                },
                "gpu_manager": {
                    "job_setup_command": {
                        "path": "/static-bin/static-bash",
                        "args": [
                            "-c",
                            "echo SETUP-GPU-OUTPUT > /gpu_setup_output_file",
                        ],
                    },
                    "test_setup_commands": True,
                },
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/layer1",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat(self):
        self.setup_files()

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                    "job_count": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"

        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/gpu_setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                    "job_count": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-GPU-OUTPUT\n"


@authors("mrkastep")
class TestSkipGpuJobSetup(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                },
                "gpu_manager": {
                    "job_setup_command": {
                        "path": "/static-bin/static-bash",
                        "args": ["-c", "echo SETUP-JUNK > /setup_output_file"],
                    },
                },
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/layer1",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat(self):
        self.setup_files()

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                    "job_count": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"


@authors("mrkastep")
class TestGpuLayer(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    NUM_SECONDARY_MASTER_CELLS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                },
                "gpu_manager": {
                    "driver_layer_directory_path": "//tmp/drivers",
                    "driver_version": "test_version",
                    "driver_layer_fetch_period": 10000,
                    "test_resource": True,
                    "test_layers": True,
                    "test_gpu_count": 1,
                },
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    USE_PORTO = True

    def setup_files(self):
        tx = start_transaction()

        create("map_node", "//tmp/drivers", tx=tx)
        create(
            "file",
            "//tmp/drivers/test_version",
            attributes={"replication_factor": 1},
            tx=tx,
        )

        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/drivers/test_version",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
            tx=tx,
        )

        create("file", "//tmp/layer2", attributes={"replication_factor": 1}, tx=tx)
        file_name = "layers/test.tar.gz"
        write_file(
            "//tmp/layer2",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
            tx=tx,
        )

        commit_transaction(tx)

    def test_setup_cat_gpu_layer(self):
        self.setup_files()

        get("//tmp/drivers/test_version/@content_revision")

        create(
            "table",
            "//tmp/t_in",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/layer2"],
                    "enable_gpu_layers": True,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"


@authors("mrkastep")
class TestGpuLayerUpdate(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    NUM_SECONDARY_MASTER_CELLS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "gpu_manager": {
                    "driver_layer_directory_path": "//tmp/drivers",
                    "driver_version": "test_version",
                    "driver_layer_fetch_period": 10000,
                    "test_resource": True,
                    "test_layers": True,
                    "test_gpu_count": 1,
                }
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    # USE_PORTO = True

    def _write_driver_layer(self, name):
        path = "layers/{}.tar.gz".format(name)
        write_file(
            "//tmp/drivers/test_version",
            open(path).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def setup_files(self):
        tx = start_transaction()

        create("map_node", "//tmp/drivers", tx=tx)
        create(
            "file",
            "//tmp/drivers/test_version",
            attributes={"replication_factor": 1},
            tx=tx,
        )

        create("file", "//tmp/bin", attributes={"replication_factor": 1}, tx=tx)
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/bin",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
            tx=tx,
        )

        commit_transaction(tx)

    def test_update_file(self):
        self.setup_files()

        create(
            "table",
            "//tmp/t_in",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )

        write_table("//tmp/t_in", [{"k": 0}])

        def check_cat(content):
            op = map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/name >&2",
                spec={
                    "max_failed_job_count": 1,
                    "mapper": {
                        "job_count": 1,
                        "layer_paths": ["//tmp/bin"],
                        "enable_gpu_layers": True,
                    },
                },
            )

            jobs_path = op.get_path() + "/jobs"
            assert get(jobs_path + "/@count") == 1
            job_id = ls(jobs_path)[0]

            res = op.read_stderr(job_id)
            return res == content

        self._write_driver_layer("olli")
        wait(partial(check_cat, "Olli Tukiainen\n"), ignore_exceptions=True)

        self._write_driver_layer("marko")
        wait(partial(check_cat, "Marko Saaresto\n"), ignore_exceptions=True)


@authors("mrkastep")
class TestCudaLayer(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    NUM_SECONDARY_MASTER_CELLS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                },
                "gpu_manager": {
                    "driver_version": "0",
                    "toolkit_min_driver_version": {"0": "0"},
                    "test_resource": True,
                    "test_layers": True,
                    "test_gpu_count": 1,
                },
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {"controller_agent": {"cuda_toolkit_layer_directory_path": "//tmp/cuda"}}

    USE_PORTO = True

    def setup_files(self, cuda_version="0"):
        create("map_node", "//tmp/cuda")

        create("file", "//tmp/cuda/" + cuda_version, attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/cuda/" + cuda_version,
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        create("file", "//tmp/layer2", attributes={"replication_factor": 1})
        file_name = "layers/test.tar.gz"
        write_file(
            "//tmp/layer2",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat_gpu_layer(self):
        self.setup_files()

        create(
            "table",
            "//tmp/t_in",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/layer2"],
                    "enable_gpu_layers": True,
                    "cuda_toolkit_version": "0",
                    "gpu_limit": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"

    def test_dynamic_config_for_cuda_toolkit_version(self):
        self.setup_files(cuda_version="1")
        update_nodes_dynamic_config({
            "exec_agent": {
                "job_controller": {
                    "gpu_manager": {
                        "toolkit_min_driver_version": {"1": "0"},
                    },
                },
            },
        })

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        write_table("//tmp/t_in", [{"k": 0}])

        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/layer2"],
                    "enable_gpu_layers": True,
                    "cuda_toolkit_version": "1",
                    "gpu_limit": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"


@authors("mrkastep")
class TestForceCudaLayer(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    NUM_SECONDARY_MASTER_CELLS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "gpu_manager": {
                    "driver_version": "0",
                    "driver_layer_directory_path": "//tmp/drivers",
                    "toolkit_min_driver_version": {"0": "0"},
                    "job_setup_command": {
                        "path": "/static-bin/static-bash",
                        "args": [
                            "-c",
                            "echo SETUP-OUTPUT > /playground/setup_output_file",
                        ],
                    },
                    "test_resource": True,
                    "test_gpu_count": 1,
                }
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {"controller_agent": {"cuda_toolkit_layer_directory_path": "//tmp/cuda"}}

    USE_PORTO = True

    def setup_files(self):
        create("map_node", "//tmp/cuda")
        create("map_node", "//tmp/drivers")

        create("file", "//tmp/cuda/0", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/cuda/0",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        create("file", "//tmp/layer2", attributes={"replication_factor": 1})
        file_name = "layers/test.tar.gz"
        write_file(
            "//tmp/layer2",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        create("file", "//tmp/drivers/0", attributes={"replication_factor": 1})
        file_name = "layers/playground.tar.gz"
        write_file(
            "//tmp/drivers/0",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat_force_gpu_layer(self):
        self.setup_files()
        with Restarter(self.Env, NODES_SERVICE):
            pass

        create(
            "table",
            "//tmp/t_in",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"replication_factor": 1},
            file_writer={"upload_replication_factor": 1},
        )

        write_table("//tmp/t_in", [{"k": 0}])

        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/playground/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/layer2"],
                    "enable_gpu_layers": True,
                    "cuda_toolkit_version": "0",
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "SETUP-OUTPUT\n"


@authors("mrkastep")
class TestSetupUser(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": [
                        "-c",
                        "/static-bin/static-id -u > /playground/setup_output_file",
                    ],
                },
                "setup_command_user": "2019",
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }
    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/layer1",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        create("file", "//tmp/playground_layer", attributes={"replication_factor": 1})
        file_name = "layers/playground.tar.gz"
        write_file(
            "//tmp/playground_layer",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat(self):
        self.setup_files()

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/playground/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1", "//tmp/playground_layer"],
                    "job_count": 1,
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "2019\n"


class TestRootFS(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 3

    USE_PORTO = True
    USE_CUSTOM_ROOTFS = True

    @authors("gritukan")
    def test_map(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"x": 1}])

        op = map(track=False, in_="//tmp/t_in", out="//tmp/t_out", command="static_cat")
        op.track()

        assert read_table("//tmp/t_out") == [{"x": 1}]

    @authors("gritukan")
    def test_vanilla(self):
        create("table", "//tmp/stderr")

        op = vanilla(
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 1,
                        "command": 'echo "task_a" >&2',
                    },
                    "task_b": {
                        "job_count": 1,
                        "command": 'echo "task_b" >&2',
                    },
                },
                "stderr_table_path": "//tmp/stderr",
            }
        )

        table_stderrs = read_table("//tmp/stderr")
        table_stderrs_per_task = Counter(row["data"] for row in table_stderrs)

        job_ids = ls(op.get_path() + "/jobs")
        cypress_stderrs_per_task = Counter(
            read_file(op.get_path() + "/jobs/{0}/stderr".format(job_id)) for job_id in job_ids
        )

        assert dict(table_stderrs_per_task) == {"task_a\n": 1, "task_b\n": 1}
        assert dict(cypress_stderrs_per_task) == {"task_a\n": 1, "task_b\n": 1}


@authors("ignat")
class TestGpuCheck(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1

    USE_PORTO = True

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "gpu_manager": {
                    "driver_version": "0",
                    "test_resource": True,
                    "test_layers": True,
                    "test_gpu_count": 1,
                },
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "gpu_check_layer_directory_path": "//tmp/gpu_check"
        }
    }

    def setup_gpu_layer_and_reset_nodes(self):
        create("map_node", "//tmp/gpu_check")

        create("file", "//tmp/gpu_check/0", attributes={"replication_factor": 1})
        file_name = "layers/gpu_check.tar.gz"
        write_file(
            "//tmp/gpu_check/0",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        create("file", "//tmp/base_layer", attributes={"replication_factor": 1})
        file_name = "rootfs/rootfs.tar.gz"
        write_file(
            "//tmp/base_layer",
            open(file_name).read(),
            file_writer={"upload_replication_factor": 1},
        )

        # Reload node to reset alerts.
        with Restarter(self.Env, NODES_SERVICE):
            pass

        wait(lambda: get("//sys/scheduler/orchid/scheduler/nodes").values()[0]["resource_limits"]["user_slots"] > 0)

    def setup_tables(self):
        create(
            "table",
            "//tmp/t_in",
            attributes={"replication_factor": 1},
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"replication_factor": 1},
        )

    def test_gpu_check_success(self):
        self.setup_gpu_layer_and_reset_nodes()

        self.setup_tables()

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo AAA >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/base_layer"],
                    "enable_gpu_layers": True,
                    "gpu_check_layer_name": "0",
                    "gpu_check_binary_path": "/gpu_check/gpu_check_success",
                },
            },
        )

        jobs_path = op.get_path() + "/jobs"
        assert get(jobs_path + "/@count") == 1
        job_id = ls(jobs_path)[0]

        res = op.read_stderr(job_id)
        assert res == "AAA\n"

    def test_gpu_check_fail(self):
        self.setup_gpu_layer_and_reset_nodes()

        self.setup_tables()

        node = ls("//sys/cluster_nodes")[0]

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo AAA >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/base_layer"],
                    "enable_gpu_layers": True,
                    "gpu_check_layer_name": "0",
                    "gpu_check_binary_path": "/gpu_check/gpu_check_fail",
                },
            },
        )

        alerts_path = "//sys/cluster_nodes/{}/@alerts".format(node)
        wait(lambda: get(alerts_path))

        alerts = get(alerts_path)
        assert len(alerts) == 1
        assert "GPU check command failed" in str(alerts[0])

        resource_limits_path = "//sys/cluster_nodes/{}/@resource_limits".format(node)
        wait(lambda: get(resource_limits_path)["user_slots"] == 0)

        wait(lambda: op.get_state() == "failed")

    def test_gpu_check_missing(self):
        self.setup_gpu_layer_and_reset_nodes()

        self.setup_tables()

        node = ls("//sys/cluster_nodes")[0]

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo AAA >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/base_layer"],
                    "enable_gpu_layers": True,
                    "gpu_check_layer_name": "0",
                    "gpu_check_binary_path": "/gpu_check/gpu_check_missing",
                },
            },
        )

        wait(lambda: op.get_state() == "failed")

        alerts_path = "//sys/cluster_nodes/{}/@alerts".format(node)
        assert len(get(alerts_path)) == 0

    def test_disable_jobs_on_gpu_check_failure(self):
        self.setup_gpu_layer_and_reset_nodes()

        self.setup_tables()

        config = {
            "%true": {
                "exec_agent": {
                    "slot_manager": {
                        "disable_jobs_on_gpu_check_failure": False
                    }
                }
            }
        }
        set("//sys/cluster_nodes/@config", config)

        node = ls("//sys/cluster_nodes")[0]

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo AAA >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "job_count": 1,
                    "layer_paths": ["//tmp/base_layer"],
                    "enable_gpu_layers": True,
                    "gpu_check_layer_name": "0",
                    "gpu_check_binary_path": "/gpu_check/gpu_check_fail",
                },
            },
        )

        alerts_path = "//sys/cluster_nodes/{}/@alerts".format(node)
        wait(lambda: get(alerts_path))

        alerts = get(alerts_path)
        assert len(alerts) == 1
        assert "GPU check command failed" in str(alerts[0])

        wait(lambda: op.get_state() == "failed")

        resource_limits_path = "//sys/cluster_nodes/{}/@resource_limits".format(node)
        assert get(resource_limits_path)["user_slots"] > 0
