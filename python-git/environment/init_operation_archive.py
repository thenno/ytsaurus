#!/usr/bin/python

import yt.yson as yson
from yt.wrapper import YtClient, TablePath, config, ypath_join
from yt.tools.dynamic_tables import make_dynamic_table_attributes, unmount_table_new, mount_table_new

import argparse
import logging
import subprocess

from yt.packages.six.moves import xrange


DEFAULT_SHARD_COUNT = 100
BASE_PATH = "//sys/operations_archive"
OPERATIONS_ARCHIVE_ACCOUNT_NAME = "operations_archive"
SYS_ACCOUNT_NAME = "sys"
RESOURCE_LIMITS_ATTRIBUTE = "resource_limits"
NODES_LIMIT_ATTRIBUTE = "node_count"

def unmount_table(client, path):
    logging.info("Unmounting table %s", path)
    unmount_table_new(client, path)

def mount_table(client, path):
    logging.info("Mounting table %s", path)
    mount_table_new(client, path)

class TableInfo(object):
    def __init__(self, key_columns, value_columns, in_memory=False, get_pivot_keys=None, attributes={}):
        def make_column(name, type_name):
            return {
                "name": name,
                "type": type_name
            }

        def make_key_column(name, type_name, expression=None):
            result = make_column(name, type_name)
            if expression:
                result["expression"] = expression
            return result

        self.schema = [make_key_column(*columns) for columns in key_columns]
        self.key_columns = [column["name"] for column in self.schema]
        self.schema += [make_column(*column) for column in value_columns]
        self.user_columns = [column["name"] for column in self.schema if "expression" not in column]
        self.get_pivot_keys = get_pivot_keys
        self.in_memory = in_memory
        self.attributes = attributes

    def create_table(self, client, path):
        attributes = make_dynamic_table_attributes(client, self.schema, self.key_columns, "scan")
        attributes.update(self.attributes)
        attributes["dynamic"] = False
        attributes["external"] = False

        logging.info("Creating table %s with attributes %s", path, attributes)
        client.create("table", path, recursive=True, attributes=attributes)

    def create_dynamic_table(self, client, path):
        attributes = make_dynamic_table_attributes(client, self.schema, self.key_columns, "scan")
        attributes.update(self.attributes)

        logging.info("Creating dynamic table %s with attributes %s", path, attributes)
        client.create("table", path, recursive=True, attributes=attributes)

    def to_dynamic_table(self, client, path):
        attributes = make_dynamic_table_attributes(client, self.schema, self.key_columns, "scan")

        # add unique_keys to schema
        attributes["schema"] = yson.to_yson_type(attributes["schema"], attributes={"unique_keys": True})

        logging.info("Sorting table %s with attributes %s", path, attributes)
        client.run_sort(path, TablePath(path, attributes=attributes), sort_by=self.key_columns, spec={
            "job_io": {"table_writer": {"block_size": 256 * 2**10, "desired_chunk_size": 100 * 2**20}},
            "force_transform": True
        })
        logging.info("Converting table to dynamic %s", path)
        client.alter_table(path, dynamic=True)
        for attr, value in self.attributes.items():
            client.set("{0}/@{1}".format(path, attr), value)

    def alter_table(self, client, path, shards, mount=True):
        logging.info("Altering table %s", path)
        unmount_table(client, path)
        attributes = make_dynamic_table_attributes(client, self.schema, self.key_columns, "scan")

        logging.info("Alter table %s with attributes %s", path, attributes)
        client.alter_table(path, schema=attributes['schema'])
        for attr, value in self.attributes.items():
            client.set("{0}/@{1}".format(path, attr), value)

        if self.get_pivot_keys:
            client.reshard_table(path, self.get_pivot_keys(shards))

        if mount:
            mount_table(client, path)

    def get_default_mapper(self):
        column_names = self.user_columns

        def default_mapper(row):
            yield dict([(key, row.get(key)) for key in column_names])

        return default_mapper

class Convert(object):
    def __init__(self, table, table_info=None, mapper=None, source=None, use_default_mapper=False):
        self.table = table
        self.table_info = table_info
        self.mapper = mapper
        self.source = source
        self.use_default_mapper = use_default_mapper

    def __call__(self, client, table_info, target_table, source_table, base_path, shard_count=DEFAULT_SHARD_COUNT, **kwargs):
        if self.table_info:
            table_info = self.table_info

        if not self.use_default_mapper and not self.mapper and not self.source and source_table:
            source_table = ypath_join(base_path, source_table)
            table_info.alter_table(client, source_table, shard_count)
            return True  # in place transformation

        source_table = self.source or source_table

        if source_table:
            source_table = ypath_join(base_path, source_table)

            if client.exists(source_table):
                table_info.create_table(client, target_table)
                mapper = self.mapper if self.mapper else table_info.get_default_mapper()
                unmount_table(client, source_table)

                logging.info("Run mapper '%s': %s -> %s", mapper.__name__, source_table, target_table)
                client.run_map(mapper, source_table, target_table)
                table_info.to_dynamic_table(client, target_table)
                client.set(target_table + "/@forced_compaction_revision", client.get(target_table + "/@revision"))
        else:
            logging.info("Creating dynamic table %s", target_table)
            table_info.create_dynamic_table(client, target_table)

        if table_info.in_memory:
            client.set(target_table + "/@in_memory_mode", "compressed")
        table_info.alter_table(client, target_table, shard_count, mount=False)
        return False  # need additional swap

class ExecAction(object):
    def __init__(self, *args):
        self.args = args

    def __call__(self, client):
        logging.info("Executing: %s", self.args)
        subprocess.check_call(self.args)

def get_default_pivots(shard_count):
    return [[]] + [[yson.YsonUint64((i * 2 ** 64) / shard_count)] for i in xrange(1, shard_count)]

TRANSFORMS = {}
ACTIONS = {}

TRANSFORMS[0] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
            ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
            ("id_hi", "uint64"),
            ("id_lo", "uint64"),
        ], [
            ("state", "string"),
            ("authenticated_user", "string"),
            ("operation_type", "string"),
            ("progress", "any"),
            ("spec", "any"),
            ("brief_progress", "any"),
            ("brief_spec", "any"),
            ("start_time", "int64"),
            ("finish_time", "int64"),
            ("filter_factors", "string"),
            ("result", "any")
        ],
        in_memory=True,
        get_pivot_keys=get_default_pivots)),
    Convert(
        "ordered_by_start_time",
        table_info=TableInfo([
            ("start_time", "int64"),
            ("id_hi", "uint64"),
            ("id_lo", "uint64")
        ], [
            ("dummy", "int64")
        ],
        in_memory=True))
]

def convert_start_finish_time_mapper(row):
    row['start_time'] = row['start_time'] + 1000000 * 60 * 60 * 3
    row['finish_time'] = row['finish_time'] + 1000000 * 60 * 60 * 3
    yield row

def convert_start_time_mapper(row):
    row['start_time'] = row['start_time'] + 1000000 * 60 * 60 * 3
    yield row

TRANSFORMS[1] = [
    Convert("ordered_by_id", mapper=convert_start_finish_time_mapper),
    Convert("ordered_by_start_time", mapper=convert_start_time_mapper)
]

TRANSFORMS[2] = [
    Convert(
        "ordered_by_start_time",
        table_info=TableInfo([
                ("start_time", "int64"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("operation_type", "string"),
                ("state", "string"),
                ("authenticated_user", "string"),
                ("filter_factors", "string")
            ],
            in_memory=True),
        source="ordered_by_id")
]

TRANSFORMS[3] = [
    Convert(
        "stderrs",
        table_info=TableInfo([
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("stderr", "string")
            ],
            get_pivot_keys=get_default_pivots)),
    Convert(
        "jobs",
        table_info=TableInfo([
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("job_type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any")
            ],
            get_pivot_keys=get_default_pivots))
]

TRANSFORMS[4] = [
    Convert(
        "jobs",
        table_info=TableInfo([
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("job_type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64")
            ],
            get_pivot_keys=get_default_pivots))
]

TRANSFORMS[5] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
                ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("state", "string"),
                ("authenticated_user", "string"),
                ("operation_type", "string"),
                ("progress", "any"),
                ("spec", "any"),
                ("brief_progress", "any"),
                ("brief_spec", "any"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("filter_factors", "string"),
                ("result", "any"),
                ("events", "any")
            ],
            in_memory=True,
            get_pivot_keys=get_default_pivots))
]

def convert_id(id):
    return id >> 32 | (((1 << 32) - 1) & id) << 32

def convert_id_mapper(row):
    row["id_hi"] = convert_id(row["id_hi"])
    row["id_lo"] = convert_id(row["id_lo"])
    yield row

def convert_job_id(row):
    row["operation_id_hi"] = convert_id(row["operation_id_hi"])
    row["operation_id_lo"] = convert_id(row["operation_id_lo"])
    row["job_id_hi"] = convert_id(row["job_id_hi"])
    row["job_id_lo"] = convert_id(row["job_id_lo"])

def convert_job_id_mapper(row):
    convert_job_id(row)
    yield row

def convert_job_id_and_job_type_mapper(row):
    convert_job_id(row)
    row["type"] = row["job_type"]
    del row["job_type"]
    yield row

TRANSFORMS[6] = [
    Convert(
        "ordered_by_id",
        mapper=convert_id_mapper),
    Convert(
        "ordered_by_start_time",
        mapper=convert_id_mapper),
    Convert(
        "jobs",
        table_info=TableInfo([
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64")
            ],
            get_pivot_keys=get_default_pivots),
        mapper=convert_job_id_and_job_type_mapper),
    Convert(
        "stderrs",
        mapper=convert_job_id_mapper)
]

def convert_statistics_remove_attributes_mapper(row):
    statistics = row['statistics']
    if hasattr(statistics):
        statistics.attributes = {}
    yield row

TRANSFORMS[7] = [
    Convert(
        "jobs",
        table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("events", "any")
            ],
            get_pivot_keys=get_default_pivots),
        use_default_mapper=True)
]

def set_table_ttl(client, path, ttl=None, auto_compaction_period=None, drow_stale_rows=False):
    table = "{0}/{1}".format(BASE_PATH, path)
    if ttl is not None:
        client.set(table + "/@max_data_ttl", ttl)
    if auto_compaction_period is not None:
        client.set(table + "/@auto_compaction_period", auto_compaction_period)
    if drow_stale_rows:
        client.set(table + "/@max_data_versions", 1)
        client.set(table + "/@min_data_ttl", 0)
    client.set(table + "/@min_data_versions", 0)
    client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
    client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
    client.remount_table(table)

def set_table_ttl_1week(path):
    def action(client):
        logging.info("Setting %s archive TTL: 1 week", path)
        set_table_ttl(client, path, ttl=1000*3600*24*7, auto_compaction_period=1000*3600*24*1)

    return action

def set_table_ttl_2years(path):
    def action(client):
        logging.info("Setting %s archive TTL: 2 years", path)
        set_table_ttl(client, path, ttl=1000*3600*24*365*2, auto_compaction_period=1000*3600*24*30)

    return action

def set_ttl_disallowing_obsolete_rows(path):
    def action(client):
        logging.info("Disallowing obsolete rows in %s archive", path)
        set_table_ttl(client, path, drow_stale_rows=True)

    return action

def remove_partition_size_options(path):
    def action(client):
        logging.info("Unsetting *_partition_data_size options in %s archive", path)
        table = "{0}/{1}".format(BASE_PATH, path)
        client.remove(table + "/@min_partition_data_size")
        client.remove(table + "/@desired_partition_data_size")
        client.remove(table + "/@max_partition_data_size")
        client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
        client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
        client.remount_table(table)

    return action

def reshard_with_fixed_pivots(path):
    def action(client):
        logging.info("Resharding %s archive with fixed pivots and disabling tablet balancer", path)
        table = "{0}/{1}".format(BASE_PATH, path)
        shard_count = 5 * client.get("//sys/tablet_cell_bundles/sys/@tablet_cell_count")
        client.set(table + "/@disable_tablet_balancer", True)
        unmount_table(client, table)
        client.reshard_table(table, get_default_pivots(shard_count))
        client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
        client.set(table + "/@forced_compaction_revision", client.get(table + "/@revision"))
        mount_table(client, table)

    return action

def create_operations_archive_account(client):
    if not client.exists("//sys/accounts/{0}".format(OPERATIONS_ARCHIVE_ACCOUNT_NAME)):
        logging.info("Creating account: %s", OPERATIONS_ARCHIVE_ACCOUNT_NAME)
        client.create("account", attributes={"name": OPERATIONS_ARCHIVE_ACCOUNT_NAME})

    limits = client.get("//sys/accounts/{0}/@{1}".format(SYS_ACCOUNT_NAME, RESOURCE_LIMITS_ATTRIBUTE))
    limits[NODES_LIMIT_ATTRIBUTE] = 100
    logging.info("Setting account limits %s", limits)
    client.set("//sys/accounts/{0}/@{1}".format(OPERATIONS_ARCHIVE_ACCOUNT_NAME, RESOURCE_LIMITS_ATTRIBUTE), limits)

def set_operations_archive_account(client):
    for obj in client.search(BASE_PATH, attributes=["account", "type", "dynamic"]):
        if obj.attributes["account"] == OPERATIONS_ARCHIVE_ACCOUNT_NAME:
            continue

        is_dynamic_table = obj.attributes["type"] == "table" and obj.attributes.get("dynamic", False)

        if is_dynamic_table:
            client.unmount_table(obj, sync=True)

        client.set(obj + "/@account", OPERATIONS_ARCHIVE_ACCOUNT_NAME)

        if is_dynamic_table:
            client.mount_table(obj, sync=True)

ACTIONS[8] = [
    create_operations_archive_account,
    set_operations_archive_account
]

TRANSFORMS[9] = [
    Convert(
        "stderrs",
        table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("stderr", "string")
            ],
            get_pivot_keys=get_default_pivots),
        use_default_mapper=True)
]

TRANSFORMS[10] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
                ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("state", "string"),
                ("authenticated_user", "string"),
                ("operation_type", "string"),
                ("progress", "any"),
                ("spec", "any"),
                ("brief_progress", "any"),
                ("brief_spec", "any"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("filter_factors", "string"),
                ("result", "any"),
                ("events", "any"),
                ("alerts", "any")
            ],
            in_memory=True,
            get_pivot_keys=get_default_pivots))
]

ACTIONS[11] = [
    set_table_ttl_1week("jobs"),
    set_table_ttl_1week("stderrs"),
    set_table_ttl_2years("ordered_by_id"),
    set_table_ttl_2years("ordered_by_start_time"),
]

def add_attributes(path, attributes):
    table = "{0}/{1}".format(BASE_PATH, path)

    def action(client):
        client.unmount_table(table, sync=True)
        logging.info("Adding attributes %s to table %s", attributes, table)
        for attribute, value in attributes.items():
            client.set("{0}/@{1}".format(table, attribute), value)
        client.mount_table(table, sync=True)

    return action

def add_sys_bundle(table):
    return add_attributes(table, {"tablet_cell_bundle": "sys"})

def ensure_sys_bundle_existence(client):
    if not client.exists("//sys/tablet_cell_bundles/sys"):
        logging.info("Creating sys bundle")
        client.create("tablet_cell_bundle", attributes={"name": "sys"})

ACTIONS[12] = [
    ensure_sys_bundle_existence,
    add_sys_bundle("ordered_by_id"),
    add_sys_bundle("ordered_by_start_time"),
    add_sys_bundle("jobs"),
    add_sys_bundle("stderrs"),
]

TRANSFORMS[13] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
                ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("state", "string"),
                ("authenticated_user", "string"),
                ("operation_type", "string"),
                ("progress", "any"),
                ("spec", "any"),
                ("brief_progress", "any"),
                ("brief_spec", "any"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("filter_factors", "string"),
                ("result", "any"),
                ("events", "any"),
                ("alerts", "any"),
                ("slot_index", "int64")
            ],
            in_memory=True,
            get_pivot_keys=get_default_pivots))
]

TRANSFORMS[14] = [
    Convert(
        "job_specs",
        table_info=TableInfo([
                ("job_id_hash", "uint64", "farm_hash(job_id_hi, job_id_lo)"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("spec", "string"),
                ("spec_version", "int64"),
            ],
            get_pivot_keys=get_default_pivots))
]

ACTIONS[14] = [
    set_table_ttl_1week("job_specs"),
    add_sys_bundle("job_specs"),
]

TRANSFORMS[15] = [
    Convert(
        "ordered_by_start_time",
        table_info=TableInfo([
                ("start_time", "int64"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("operation_type", "string"),
                ("state", "string"),
                ("authenticated_user", "string"),
                ("filter_factors", "string"),
                ("pool", "string")
            ],
            in_memory=True))
]

TRANSFORMS[16] = [
    Convert(
        "jobs",
            table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("events", "any"),
                ("transient_state", "string"),
            ],
            attributes={"atomicity": "none"})),
    Convert(
        "job_specs",
        table_info=TableInfo([
                ("job_id_hash", "uint64", "farm_hash(job_id_hi, job_id_lo)"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64"),
            ], [
                ("spec", "string"),
                ("spec_version", "int64"),
                ("type", "string"),
            ],
            attributes={"atomicity": "none"})),
]

TRANSFORMS[17] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
                ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("state", "string"),
                ("authenticated_user", "string"),
                ("operation_type", "string"),
                ("progress", "any"),
                ("spec", "any"),
                ("brief_progress", "any"),
                ("brief_spec", "any"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("filter_factors", "string"),
                ("result", "any"),
                ("events", "any"),
                ("alerts", "any"),
                ("slot_index", "int64"),
                ("unrecognized_spec", "any"),
                ("full_spec", "any"),
            ],
            in_memory=True,
            get_pivot_keys=get_default_pivots))
]

TRANSFORMS[18] = [
    Convert(
        "jobs",
            table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("events", "any"),
                ("transient_state", "string"),
                ("update_time", "int64"),
            ],
            attributes={"atomicity": "none"})),
]

TRANSFORMS[19] = [
    Convert(
        "stderrs",
        table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("stderr", "string")
            ],
            attributes={"atomicity": "none"}))
]

TRANSFORMS[20] = [
    Convert(
        "jobs",
            table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("has_spec", "boolean"),
                ("events", "any"),
                ("transient_state", "string"),
                ("update_time", "int64"),
            ],
            attributes={"atomicity": "none"})),
]

TRANSFORMS[21] = [
    Convert(
        "fail_contexts",
        table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("fail_context", "string")
            ],
            attributes={"atomicity": "none"}),
        use_default_mapper=True),
    Convert(
        "jobs",
            table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("has_spec", "boolean"),
                ("has_fail_context", "boolean"),
                ("events", "any"),
                ("transient_state", "string"),
                ("update_time", "int64"),
            ],
            attributes={"atomicity": "none"})),
]

TRANSFORMS[22] = [
    Convert(
        "ordered_by_id",
        table_info=TableInfo([
            ("id_hash", "uint64", "farm_hash(id_hi, id_lo)"),
            ("id_hi", "uint64"),
            ("id_lo", "uint64"),
        ], [
            ("state", "string"),
            ("authenticated_user", "string"),
            ("operation_type", "string"),
            ("progress", "any"),
            ("spec", "any"),
            ("brief_progress", "any"),
            ("brief_spec", "any"),
            ("start_time", "int64"),
            ("finish_time", "int64"),
            ("filter_factors", "string"),
            ("result", "any"),
            ("events", "any"),
            ("alerts", "any"),
            ("slot_index", "int64"),
            ("unrecognized_spec", "any"),
            ("full_spec", "any"),
            ("runtime_parameters", "any")
        ],
            in_memory=True,
            get_pivot_keys=get_default_pivots))
]

TRANSFORMS[23] = [
    Convert(
        "jobs",
            table_info=TableInfo([
                ("operation_id_hash", "uint64", "farm_hash(operation_id_hi, operation_id_lo)"),
                ("operation_id_hi", "uint64"),
                ("operation_id_lo", "uint64"),
                ("job_id_hi", "uint64"),
                ("job_id_lo", "uint64")
            ], [
                ("type", "string"),
                ("state", "string"),
                ("start_time", "int64"),
                ("finish_time", "int64"),
                ("address", "string"),
                ("error", "any"),
                ("statistics", "any"),
                ("stderr_size", "uint64"),
                ("spec", "string"),
                ("spec_version", "int64"),
                ("has_spec", "boolean"),
                ("has_fail_context", "boolean"),
                ("fail_context_size", "uint64"),
                ("events", "any"),
                ("transient_state", "string"),
                ("update_time", "int64"),
            ],
            attributes={"atomicity": "none"})),
]

ACTIONS[23] = [
    set_table_ttl_1week("stderrs"),
    set_table_ttl_1week("fail_contexts"),
]

TRANSFORMS[24] = [
    Convert(
        "ordered_by_start_time",
        table_info=TableInfo([
                ("start_time", "int64"),
                ("id_hi", "uint64"),
                ("id_lo", "uint64"),
            ], [
                ("operation_type", "string"),
                ("state", "string"),
                ("authenticated_user", "string"),
                ("filter_factors", "string"),
                ("pool", "string"),
                ("pools", "any"),
                ("has_failed_jobs", "boolean"),
            ],
            in_memory=True)),
]

ACTIONS[25] = [
    set_ttl_disallowing_obsolete_rows("fail_contexts"),
    set_ttl_disallowing_obsolete_rows("job_specs"),
    set_ttl_disallowing_obsolete_rows("jobs"),
    set_ttl_disallowing_obsolete_rows("ordered_by_id"),
    set_ttl_disallowing_obsolete_rows("ordered_by_start_time"),
    set_ttl_disallowing_obsolete_rows("stderrs"),

    remove_partition_size_options("fail_contexts"),
    remove_partition_size_options("job_specs"),
    remove_partition_size_options("jobs"),
    remove_partition_size_options("ordered_by_id"),
    remove_partition_size_options("ordered_by_start_time"),
    remove_partition_size_options("stderrs"),

    reshard_with_fixed_pivots("jobs"),
    reshard_with_fixed_pivots("ordered_by_id"),
]

def swap_table(client, target, source, version):
    backup_path = target + ".bak.{0}".format(version)
    has_target = False
    if client.exists(target):
        has_target = True
        unmount_table(client, target)

    unmount_table(client, source)

    logging.info("Swapping tables %s <-> %s", source, target)
    if has_target:
        client.move(target, backup_path)
    client.move(source, target)

    mount_table(client, target)

def transform_archive(client, transform_begin, transform_end, force, archive_path, **kwargs):
    logging.info("Transforming archive from %s to %s version", transform_begin - 1, transform_end)
    schemas = {}
    for version in xrange(0, transform_begin):
        if version in TRANSFORMS:
            for convertion in TRANSFORMS[version]:
                table = convertion.table
                if convertion.table_info:
                    schemas[table] = convertion.table_info

    for version in xrange(transform_begin, transform_end + 1):
        logging.info("Transforming to version %d", version)
        swap_tasks = []
        had_work = False
        if version in TRANSFORMS:
            for convertion in TRANSFORMS[version]:
                had_work = True
                table = convertion.table
                tmp_path = "{0}/{1}.tmp.{2}".format(archive_path, table, version)
                if force and client.exists(tmp_path):
                    client.remove(tmp_path)
                in_place = convertion(client, schemas.get(table), tmp_path, table if table in schemas else None, archive_path, **kwargs)
                if not in_place:
                    swap_tasks.append((ypath_join(archive_path, table), tmp_path))
                if convertion.table_info:
                    schemas[table] = convertion.table_info

            for target_path, tmp_path in swap_tasks:
                swap_table(client, target_path, tmp_path, version)

        if version in ACTIONS:
            for action in ACTIONS[version]:
                had_work = True
                action(client)

        if not had_work:
            raise ValueError("Version {0} must have actions or transformations".format(version))
        client.set_attribute(archive_path, "version", version)

def create_tables(client, target_version, shards=1, base_path=BASE_PATH):
    """ Creates operation archive tables of given version """
    assert target_version in TRANSFORMS

    schemas = {}
    for version in sorted(TRANSFORMS.keys()):
        if version > target_version:
            break
        for convertion in TRANSFORMS[version]:
            if convertion.table_info:
                schemas[convertion.table] = convertion.table_info
    for table in schemas:
        table_path = BASE_PATH + "/" + table
        schemas[table].create_dynamic_table(client, table_path)
        schemas[table].alter_table(client, table_path, shards)

    client.set(BASE_PATH + "/@version", target_version)

def create_tables_latest_version(client, shards=1, base_path=BASE_PATH):
    """ Creates operation archive tables of latest version """
    latest_version = max(TRANSFORMS.keys())
    create_tables(client, latest_version, shards=shards, base_path=base_path)

def main():
    parser = argparse.ArgumentParser(description="Transform operations archive")
    parser.add_argument("--target-version", type=int, required=True)
    parser.add_argument("--force", action="store_true", default=False)
    parser.add_argument("--archive-path", type=str, default=BASE_PATH)
    parser.add_argument("--shard-count", type=int, default=DEFAULT_SHARD_COUNT)
    parser.add_argument("--latest", action="store_true")

    args = parser.parse_args()

    archive_path = args.archive_path

    logging.basicConfig(format="%(asctime)s - %(levelname)s - %(message)s", level=logging.INFO)

    client = YtClient(proxy=config["proxy"]["url"], token=config["token"])

    client.config['pickling']['module_filter'] = lambda module: 'hashlib' not in getattr(module, '__name__', '')

    if client.exists(archive_path):
        current_version = client.get("{0}/@".format(archive_path)).get("version", -1)
    else:
        current_version = -1

    next_version = current_version + 1

    if args.latest:
        create_tables_latest_version(client)
    else:
        target_version = args.target_version
        transform_archive(client, next_version, target_version, args.force, archive_path, shard_count=args.shard_count)


if __name__ == "__main__":
    main()
