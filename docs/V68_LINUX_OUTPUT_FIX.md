# V68 Linux 唯一输入输出修复

V65 起定义了 `SRB_SKIP_REPEAT_PROBE`，但 Linux `output_fd` 的打开代码错误地位于
`#ifndef SRB_SKIP_REPEAT_PROBE` 内。V67 在 10M 唯一请求上完成 94 个 Atlas 扩展源后，
因 `output_fd == -1` 报 `cannot start output`。

V68 仅把输出文件打开移到条件编译之外；唯一输入仍用 `O_WRONLY`。预测器、Atlas 数据、解析、
批处理和格式化均未改变。首次失败证据：GitHub Actions run `40299305315`。
