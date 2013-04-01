#!/usr/bin/env python

from atomic import process_tasks_from_list

import yt.wrapper as yt

import os
import sh
import sys
import traceback
import subprocess
from argparse import ArgumentParser
from urllib import quote_plus

yt.config.MEMORY_LIMIT = 2500 * yt.config.MB
#yt.config.DETACHED = True

def main():
    parser = ArgumentParser()
    parser.add_argument("--tables")
    parser.add_argument("--destination")
    parser.add_argument("--server")
    parser.add_argument("--import-type", default="pull")
    parser.add_argument("--proxy", action="append")
    parser.add_argument("--server-port", default="8013")
    parser.add_argument("--http-port", default="13013")
    parser.add_argument("--job-size", type=int, default=2 * 1024 ** 3)
    parser.add_argument("--job-count", type=int)
    parser.add_argument("--speed", type=int)
    parser.add_argument("--codec")
    parser.add_argument("--force", action="store_true", default=False)
    parser.add_argument("--ignore", action="store_true", default=False)
    parser.add_argument("--fastbone", action="store_true", default=False)
    parser.add_argument("--debug", action="store_true", default=False)
    parser.add_argument("--pool")
    parser.add_argument("--mapreduce-binary", default="./mapreduce")
    parser.add_argument("--yt-binary")

    args = parser.parse_args()

    use_fastbone = "-opt net_table=fastbone" if args.fastbone else ""

    def field_from_page(table, field):
        """ Extract value of given field from http page of the table """
        http_content = sh.curl("{}:{}/debug?info=table&table={}".format(args.server, args.http_port, table)).stdout
        records_line = filter(lambda line: line.find(field) != -1,  http_content.split("\n"))[0]
        records_line = records_line.replace("</b>", "").replace(",", "")
        return records_line.split(field + ":")[1].split()[0]

    def records_count(table):
        return int(field_from_page(table, "Records"))
    
    def data_size(table):
        return int(field_from_page(table, "Size"))

    def is_sorted(table):
        return field_from_page(table, "Size").lower() == "yes"

    def is_empty(table):
        """ Parse whether table is empty from html """
        http_content = sh.curl("{}:{}/debug?info=table&table={}".format(args.server, args.http_port, table)).stdout
        empty_lines = filter(lambda line: line.find("is empty") != -1,  http_content.split("\n"))
        return empty_lines and empty_lines[0].startswith("Table is empty")

    def pull_table(source, destination, count):
        has_proxy = args.proxy is not None
        if has_proxy:
            servers = ["%s:%s" % (proxy, args.http_port) for proxy in args.proxy]
        else:
            servers = ["%s:%s" % (args.server, args.server_port)]
        ranges = []
    
        record_threshold = max(1, count * args.job_size / data_size(source))
        for i in xrange((count - 1) / record_threshold + 1):
            server = servers[i % len(servers)]
            start = i * record_threshold
            end = min(count, (i + 1) * record_threshold)
            ranges.append((server, start, end))

        temp_table = yt.create_temp_table(prefix=os.path.basename(source))
        yt.write_table(temp_table,
                       ["\t".join(map(str, range)) + "\n" for range in ranges],
                       format=yt.YamrFormat(lenval=False, has_subkey=True))

        pool = args.pool
        if pool is None:
            pool = "restricted"
        spec = {"data_size_per_job": 1, "pool": pool}
        if args.job_count is not None:
            spec["job_count"] = args.job_count

        yt.set_attribute(destination, "compression_codec", "gzip_best_compression")

        if has_proxy:
            command = 'curl "http://${{server}}/table/{}?subkey=1&lenval=1&startindex=${{start}}&endindex=${{end}}"'.format(quote_plus(source))
        else:
            command = 'USER=yt MR_USER=tmp ./mapreduce -server $server {} -read {}:[$start,$end] -lenval -subkey'.format(use_fastbone, source)

        debug_str = 'echo "{}" 1>&2; '.format(command.replace('"', "'")) if args.debug else ''

        yt.run_map(
                'while true; do '
                    'IFS="\t" read -r server start end; '
                    'if [ "$?" != "0" ]; then break; fi; '
                    'set -e; '
                    '{0}'
                    '{1}; '
                    'set +e; '
                'done;'.format(debug_str, command),
                temp_table,
                destination,
                input_format=yt.YamrFormat(lenval=False, has_subkey=True),
                output_format=yt.YamrFormat(lenval=True, has_subkey=True),
                files=args.mapreduce_binary,
                spec=spec)

    def push_table(source, destination, count):
        if args.job_count is None:
            record_threshold = max(1, count * args.job_size / data_size(source))
            # Number of data pieces
            args.job_count = (count - 1) / record_threshold + 1

        if args.speed is not None:
            limit = args.speed * yt.config.MB / args.job_count
            speed_limit = "pv -q -L {} | ".format(limit)
        else:
            speed_limit = ""

        if args.yt_binary is None:
            with open("./mapreduce-yt", "w") as f:
                for block in yt.download_file("//home/files/mapreduce-yt", response_type="iter_content"):
                    f.write(block)
            args.yt_binary = "./mapreduce-yt"

        if args.fastbone:
            yt_server = "proxy-fb.yt.yandex.net"
        else:
            yt_server = "proxy.yt.yandex.net"

        subprocess.check_call(
            "MR_USER=gemini {} -server {}:{} "
                "-map '{} YT_USE_TOKEN=0 YT_USE_HOSTS=1 ./{} -server {} -append -lenval -subkey -write {}' "
                "-src {} "
                "-dst {} "
                "-jobcount {} "
                "-lenval "
                "-subkey "
                "-file {} "\
                    .format(
                        args.mapreduce_binary,
                        args.server,
                        args.server_port,
                        speed_limit,
                        os.path.basename(args.yt_binary),
                        yt_server,
                        destination,
                        source,
                        os.path.join("tmp", os.path.basename(source)),
                        args.job_count,
                        args.yt_binary),
            shell=True)
        
        if args.codec is not None:
            yt.set_attribute(destination, "compression_codec", args.codec)
            yt.run_merge(
                destination,
                destination,
                "unordered",
                spec={"combine_chunks": "true"})


    def import_table(table):
        if is_empty(table):
            print >>sys.stderr, "Table is empty"
            return -1

        count = records_count(table)
        sorted = is_sorted(table)

        destination = os.path.join(args.destination, table)
        if yt.exists(destination):
            if args.force or (yt.get_type(destination) == "table" and yt.is_empty(destination)):
                yt.remove(destination)
        if yt.exists(destination) and args.ignore:
            return
        yt.create_table(destination, recursive=True)

        try:
            if args.import_type == "pull":
                pull_table(table, destination, count)
            elif args.import_type == "push":
                push_table(table, destination, count)
            else:
                raise yt.YtError("Incorrect import type: " + args.import_type)

            # TODO: add checksum checking
            if yt.records_count(destination) != count:
                raise yt.YtError("Incorrect record count: expected=%d, actual=%d" % (count, yt.records_count(destination)))

            if sorted:
                yt.run_sort(destination, sort_by=["key", "subkey"])
        except yt.YtError:
            _, _, exc_traceback = sys.exc_info()
            traceback.print_tb(exc_traceback, file=sys.stdout)
            yt.remove(destination, force=True)

    process_tasks_from_list(
        args.tables,
        import_table
    )

if __name__ == "__main__":
    main()
