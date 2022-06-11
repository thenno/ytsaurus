from yt_env_setup import YTEnvSetup

from yt_commands import (
    authors, create, get, get_singular_chunk_id, lookup_rows, write_table, read_table)

import yt.yson as yson

import yt_proto.yt.client.chunk_client.proto.chunk_meta_pb2 as chunk_meta_pb2

##################################################################


class TestSequoiaObjects(YTEnvSetup):
    USE_SEQUOIA = True

    @authors("gritukan")
    def test_estimated_creation_time(self):
        object_id = "543507cc-00000000-12345678-abcdef01"
        creation_time = {'min': '2012-12-21T08:34:56.000000Z', 'max': '2012-12-21T08:34:57.000000Z'}
        assert get("//sys/estimated_creation_time/{}".format(object_id)) == creation_time

    @authors("gritukan")
    def test_sequoia_chunk(self):
        create("table", "//tmp/t")
        write_table("//tmp/t", [{"x": 1}])
        assert read_table("//tmp/t") == [{"x": 1}]

        chunk_id = get_singular_chunk_id("//tmp/t")
        assert get("#{}/@sequoia".format(chunk_id))
        assert get("#{}/@aevum".format(chunk_id)) != "none"

        assert len(lookup_rows("//sys/sequoia/chunk_meta_extensions", [{"id": chunk_id}])) == 1

    @authors("aleksandra-zh")
    def test_confirm_sequoia_chunk(self):
        create("table", "//tmp/t")
        write_table("//tmp/t", [{"x": 1}])
        assert read_table("//tmp/t") == [{"x": 1}]

        chunk_id = get_singular_chunk_id("//tmp/t")
        assert get("#{}/@sequoia".format(chunk_id))
        assert get("#{}/@aevum".format(chunk_id)) != "none"

        exts = lookup_rows("//sys/sequoia/chunk_meta_extensions", [{"id": chunk_id}])
        assert len(exts) == 1
        raw_misc_ext = yson.get_bytes(exts[0]["misc_ext"])
        misc_ext = chunk_meta_pb2.TMiscExt()
        misc_ext.ParseFromString(raw_misc_ext)
        assert misc_ext.row_count == 1
