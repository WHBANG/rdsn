include "../dsn.thrift"

namespace cpp dsn.service

struct copy_request
{
    1: dsn.rpc_address source;
    2: string source_dir;
    3: string dst_dir;
    4: string file_name;
    5: i64 offset;
    6: i32 size;
    7: bool is_last;
    8: bool overwrite;
    9: optional string source_disk_tag;
}

struct copy_response
{
    1: dsn.error_code error;
    2: dsn.blob file_content;
    3: i64 offset;
    4: i32 size;
}

struct get_file_size_request
{
    1: dsn.rpc_address source;
    2: string dst_dir;
    3: list<string> file_list;
    4: string source_dir;
    5: bool overwrite;
    6: optional string source_disk_tag;
    7: optional string dest_disk_tag;
}

struct get_file_size_response
{
    1: i32 error;
    2: list<string> file_list;
    3: list<i64> size_list;
}
