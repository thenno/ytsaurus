from .driver import make_request
from .table_helpers import _prepare_format, _to_chunk_stream
from .common import set_param, bool_to_string, require, is_master_transaction, YtError, get_value
from .config import get_config, get_option, get_command_param
from .cypress_commands import get
from .transaction_commands import _make_transactional_request
from .ypath import TablePath
from .http_helpers import get_retriable_errors
from .transaction import null_transaction_id
from .retries import Retrier

try:
    from cStringIO import StringIO as BytesIO
except ImportError:  # Python 3
    from io import BytesIO

import yt.logger as logger

from copy import deepcopy
import time

SYNC_LAST_COMMITED_TIMESTAMP = 0x3fffffffffffff01
ASYNC_LAST_COMMITED_TIMESTAMP = 0x3fffffffffffff04

def _waiting_for_tablets(path, state, first_tablet_index=None, last_tablet_index=None, client=None):
    tablet_count = get(path + "/@tablet_count", client=client)
    first_tablet_index = get_value(first_tablet_index, 0)
    last_tablet_index = get_value(last_tablet_index, tablet_count - 1)

    is_tablets_ready = lambda: all(tablet["state"] == state for tablet in
                                   get(path + "/@tablets", client=client)[first_tablet_index:last_tablet_index + 1])

    check_interval = get_config(client)["tablets_check_interval"] / 1000.0
    timeout = get_config(client)["tablets_ready_timeout"] / 1000.0

    start_time = time.time()
    while not is_tablets_ready():
        if time.time() - start_time > timeout:
            raise YtError("Timed out while waiting for tablets")

        time.sleep(check_interval)

def _check_transaction_type(client):
    transaction_id = get_command_param("transaction_id", client=client)
    if transaction_id == null_transaction_id:
        return
    require(not is_master_transaction(transaction_id),
            lambda: YtError("Dynamic table commands can not be performed under master transaction"))

class DynamicTableRequestRetrier(Retrier):
    def __init__(self, retry_config, command, params, data=None, client=None):
        request_timeout = get_config(client)["proxy"]["heavy_request_timeout"]
        chaos_monkey_enable = get_option("_ENABLE_HEAVY_REQUEST_CHAOS_MONKEY", client)
        super(DynamicTableRequestRetrier, self).__init__(
            retry_config=retry_config,
            timeout=request_timeout,
            exceptions=get_retriable_errors(),
            chaos_monkey_enable=chaos_monkey_enable)

        self.request_timeout = request_timeout
        self.params = params
        self.command = command
        self.client = client
        self.data = data

    def action(self):
        kwargs = {}
        if self.data is not None:
            kwargs["data"] = self.data

        response = _make_transactional_request(
            self.command,
            self.params,
            return_content=True,
            use_heavy_proxy=True,
            decode_content=False,
            timeout=self.request_timeout,
            client=self.client,
            **kwargs)

        if response is not None:
            return BytesIO(response)

    def except_action(self, error, attempt):
        logger.warning('Request "%s" has failed with error %s, message: %s',
                        self.command, str(type(error)), str(error))

def select_rows(query, timestamp=None, input_row_limit=None, output_row_limit=None, range_expansion_limit=None,
                fail_on_incomplete_result=None, verbose_logging=None, enable_code_cache=None, max_subqueries=None,
                workload_descriptor=None, format=None, raw=None, client=None):
    """Executes a SQL-like query on dynamic table.

    .. seealso:: `supported features <https://wiki.yandex-team.ru/yt/userdoc/queries>`_

    :param str query: for example \"<columns> [as <alias>], ... from \[<table>\] \
                  [where <predicate> [group by <columns> [as <alias>], ...]]\".
    :param int timestamp: timestamp.
    :param format: output format.
    :type format: str or descendant of :class:`Format <yt.wrapper.format.Format>`
    :param bool raw: don't parse response to rows.
    """
    if raw is None:
        raw = get_config(client)["default_value_of_raw_option"]
    format = _prepare_format(format, raw, client)
    params = {
        "query": query,
        "output_format": format.to_yson_type()}
    set_param(params, "timestamp", timestamp)
    set_param(params, "input_row_limit", input_row_limit)
    set_param(params, "output_row_limit", output_row_limit)
    set_param(params, "range_expansion_limit", range_expansion_limit)
    set_param(params, "fail_on_incomplete_result", fail_on_incomplete_result, transform=bool_to_string)
    set_param(params, "verbose_logging", verbose_logging, transform=bool_to_string)
    set_param(params, "enable_code_cache", enable_code_cache, transform=bool_to_string)
    set_param(params, "max_subqueries", max_subqueries)
    set_param(params, "workload_descriptor", workload_descriptor)

    _check_transaction_type(client)

    response = DynamicTableRequestRetrier(
        get_config(client)["dynamic_table_retries"],
        "select_rows",
        params,
        client=client).run()

    if raw:
        return response
    else:
        return format.load_rows(response)

def insert_rows(table, input_stream, update=None, aggregate=None, atomicity=None, durability=None,
                format=None, raw=None, require_sync_replica=None, client=None):
    """Inserts rows from input_stream to dynamic table.

    :param table: output table path.
    :type table: str or :class:`TablePath <yt.wrapper.ypath.TablePath>`
    :param input_stream: python file-like object, string, list of strings.
    :param format: format of input data, ``yt.wrapper.config["tabular_data_format"]`` by default.
    :type format: str or descendant of :class:`Format <yt.wrapper.format.Format>`
    :param bool raw: if `raw` is specified stream with unparsed records (strings) \
    in specified `format` is expected. Otherwise dicts or :class:`Record <yt.wrapper.yamr_record.Record>` \
    are expected.
    :param bool require_sync_replica: require sync replica write.
    """
    if raw is None:
        raw = get_config(client)["default_value_of_raw_option"]

    table = TablePath(table, client=client)
    format = _prepare_format(format, raw, client)

    params = {}
    params["path"] = table
    params["input_format"] = format.to_yson_type()
    set_param(params, "update", update, transform=bool_to_string)
    set_param(params, "aggregate", aggregate, transform=bool_to_string)
    set_param(params, "atomicity", atomicity)
    set_param(params, "durability", durability)
    set_param(params, "require_sync_replica", require_sync_replica)

    input_data = b"".join(_to_chunk_stream(input_stream, format, raw, split_rows=False,
                                           chunk_size=get_config(client)["write_retries"]["chunk_size"]))

    retry_config = deepcopy(get_config(client)["dynamic_table_retries"])
    retry_config["enable"] = retry_config["enable"] and \
        not aggregate and get_command_param("transaction_id", client) == null_transaction_id

    _check_transaction_type(client)

    DynamicTableRequestRetrier(
        retry_config,
        "insert_rows",
        params,
        data=input_data,
        client=client).run()

def delete_rows(table, input_stream, atomicity=None, durability=None, format=None, raw=None,
                require_sync_replica=None, client=None):
    """Deletes rows with keys from input_stream from dynamic table.

    :param table: table to remove rows from.
    :type table: str or :class:`TablePath <yt.wrapper.ypath.TablePath>`
    :param input_stream: python file-like object, string, list of strings.
    :param format: format of input data, ``yt.wrapper.config["tabular_data_format"]`` by default.
    :type format: str or descendant of :class:`Format <yt.wrapper.format.Format>`
    :param bool raw: if `raw` is specified stream with unparsed records (strings) \
    in specified `format` is expected. Otherwise dicts or :class:`Record <yt.wrapper.yamr_record.Record>` \
    are expected.
    :param bool require_sync_replica: require sync replica write.
    """
    if raw is None:
        raw = get_config(client)["default_value_of_raw_option"]

    table = TablePath(table, client=client)
    format = _prepare_format(format, raw, client)

    params = {}
    params["path"] = table
    params["input_format"] = format.to_yson_type()
    set_param(params, "atomicity", atomicity)
    set_param(params, "durability", durability)
    set_param(params, "require_sync_replica", require_sync_replica)

    input_data = b"".join(_to_chunk_stream(input_stream, format, raw, split_rows=False,
                                           chunk_size=get_config(client)["write_retries"]["chunk_size"]))

    retry_config = deepcopy(get_config(client)["dynamic_table_retries"])
    retry_config["enable"] = retry_config["enable"] and \
        get_command_param("transaction_id", client) == null_transaction_id

    _check_transaction_type(client)

    DynamicTableRequestRetrier(
        retry_config,
        "delete_rows",
        params,
        data=input_data,
        client=client).run()

def lookup_rows(table, input_stream, timestamp=None, column_names=None, keep_missing_rows=None,
                format=None, raw=None, client=None):
    """Lookups rows in dynamic table.

    .. seealso:: `supported features <https://wiki.yandex-team.ru/yt/userdoc/queries>`_

    :param format: output format.
    :type format: str or descendant of :class:`Format <yt.wrapper.format.Format>`
    :param bool raw: don't parse response to rows.
    """
    if raw is None:
        raw = get_config(client)["default_value_of_raw_option"]

    table = TablePath(table, client=client)
    format = _prepare_format(format, raw, client)

    params = {}
    params["path"] = table
    params["input_format"] = format.to_yson_type()
    params["output_format"] = format.to_yson_type()
    set_param(params, "timestamp", timestamp)
    set_param(params, "column_names", column_names)
    set_param(params, "keep_missing_rows", keep_missing_rows, transform=bool_to_string)

    input_data = b"".join(_to_chunk_stream(input_stream, format, raw, split_rows=False,
                                           chunk_size=get_config(client)["write_retries"]["chunk_size"]))

    _check_transaction_type(client)

    response = DynamicTableRequestRetrier(
        get_config(client)["dynamic_table_retries"],
        "lookup_rows",
        params,
        data=input_data,
        client=client).run()

    if raw:
        return response
    else:
        return format.load_rows(response)

def mount_table(path, first_tablet_index=None, last_tablet_index=None, cell_id=None,
                freeze=False, sync=False, client=None):
    """Mounts table.

    TODO
    """
    if sync and get_option("_client_type", client) == "batch":
        raise YtError("Sync mode is not available with batch client")

    path = TablePath(path, client=client)
    params = {"path": path}
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)
    set_param(params, "cell_id", cell_id)
    set_param(params, "freeze", freeze)

    make_request("mount_table", params, client=client)

    if sync:
        state = "frozen" if freeze else "mounted"
        _waiting_for_tablets(path, state, first_tablet_index, last_tablet_index, client)

def unmount_table(path, first_tablet_index=None, last_tablet_index=None, force=None, sync=False, client=None):
    """Unmounts table.

    TODO
    """
    if sync and get_option("_client_type", client) == "batch":
        raise YtError("Sync mode is not available with batch client")

    params = {"path": TablePath(path, client=client)}
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)
    set_param(params, "force", force)

    make_request("unmount_table", params, client=client)

    if sync:
        _waiting_for_tablets(path, "unmounted", first_tablet_index, last_tablet_index, client)

def remount_table(path, first_tablet_index=None, last_tablet_index=None, client=None):
    """Remounts table.

    TODO
    """
    params = {"path": path}
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)

    make_request("remount_table", params, client=client)


def freeze_table(path, first_tablet_index=None, last_tablet_index=None, sync=False, client=None):
    """Freezes table.

    TODO
    """
    if sync and get_option("_client_type", client) == "batch":
        raise YtError("Sync mode is not available with batch client")

    params = {"path": TablePath(path, client=client)}
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)

    make_request("freeze_table", params, client=client)

    if sync:
        _waiting_for_tablets(path, "frozen", first_tablet_index, last_tablet_index, client)

def unfreeze_table(path, first_tablet_index=None, last_tablet_index=None, sync=False, client=None):
    """Unfreezes table.

    TODO
    """
    if sync and get_option("_client_type", client) == "batch":
        raise YtError("Sync mode is not available with batch client")

    params = {"path": TablePath(path, client=client)}
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)

    make_request("unfreeze_table", params, client=client)

    if sync:
        _waiting_for_tablets(path, "mounted", first_tablet_index, last_tablet_index, client)


def reshard_table(path, pivot_keys=None, tablet_count=None, first_tablet_index=None, last_tablet_index=None, client=None):
    """Changes pivot keys separating tablets of a given table.

    TODO
    """
    params = {"path": TablePath(path, client=client)}

    set_param(params, "pivot_keys", pivot_keys)
    set_param(params, "tablet_count", tablet_count)
    set_param(params, "first_tablet_index", first_tablet_index)
    set_param(params, "last_tablet_index", last_tablet_index)

    make_request("reshard_table", params, client=client)

def trim_rows(path, tablet_index, trimmed_row_count, client=None):
    """Trim rows of the dynamic table.

    :param path: path to table.
    :type path: str or :class:`TablePath <yt.wrapper.ypath.TablePath>`
    :param int tablet_index: tablet index.
    :param int trimmed_row_count: trimmed row count.
    """

    params = {"path": TablePath(path, client=client)}

    set_param(params, "tablet_index", tablet_index)
    set_param(params, "trimmed_row_count", trimmed_row_count)

    make_request("trim_rows", params, client=client)

def alter_table_replica(replica_id, enabled=None, mode=None, client=None):
    """TODO"""
    if mode is not None:
        require(mode in ("sync", "async"), lambda: YtError("Invalid mode. Expected sync or async"))

    params = {"replica_id": replica_id}
    set_param(params, "mode", mode)
    set_param(params, "enabled", enabled)

    return make_request("alter_table_replica", params, client=client)
