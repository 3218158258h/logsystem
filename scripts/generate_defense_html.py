#!/usr/bin/env python3
"""Build a single-file IPC log system defense deck from the supplied HTML template."""

from pathlib import Path
from html.parser import HTMLParser
import re


ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "【模板】六环实习总结网页版.html"
OUTPUT = ROOT / "答辩材料" / "IPC日志系统答辩-2026-10-28.html"

SLIDES_HTML = r"""
<!-- SLIDE 1: Cover -->
<div class="slide slide-cover active" data-layout="cover">
    <div class="cover-title">IPC 日志系统</div>
    <div class="cover-subtitle">设计与实现 · 多进程异步日志汇聚</div>
    <div class="cover-meta">
        <div>项目：<span class="cover-meta-item">HJM-fast</span></div>
        <div>技术栈：<span class="cover-meta-item">C++11 · Linux · FDBus 5.4.0 · Protobuf</span></div>
        <div>答辩材料版本：<span class="cover-meta-item">2026.10.28</span></div>
    </div>
    <div class="cover-stamp">技术答辩 <span>15 slides</span></div>
</div>

<!-- SLIDE 2: Background -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">项目背景｜让多进程日志可汇聚、可追溯</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two">
            <div class="deck-card accent-blue">
                <div class="eyebrow">WHY · 现场问题</div>
                <div class="card-heading">日志分散，排查链路容易断</div>
                <ul class="compact-list">
                    <li>多个业务进程各自输出，时间线分散，定位跨进程事件需要人工拼接。</li>
                    <li>如果业务线程同步写盘或等待网络，日志本身可能拖慢业务。</li>
                    <li>动态排障需要分别调整进程等级，不能把等级控制绑死在统一配置中心。</li>
                </ul>
            </div>
            <div class="deck-card accent-orange">
                <div class="eyebrow">WHAT · 设计目标</div>
                <div class="card-heading">形成一条可解释的日志通路</div>
                <div class="mini-flow">
                    <span>业务进程</span><b>异步上报</b><span>日志主进程</span><b>统一输出</b><span>终端 / 文件</span>
                </div>
                <div class="tag-row">
                    <span class="tag">四级过滤</span><span class="tag">毫秒时间戳</span>
                    <span class="tag">有界队列</span><span class="tag">受控丢弃计数</span>
                </div>
                <div class="callout">核心问题：怎样在可控资源、可解释边界和业务线程响应之间取得平衡？</div>
            </div>
        </div>
        <div class="anchorline">题目依据：第 2 节功能需求；整体目标见 <code>项目架构.md</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>02</div></div>
</div>

<!-- SLIDE 3: Requirements -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">题目要求｜逐项映射到实现边界</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two requirements-grid">
            <div class="deck-card requirement-card">
                <div class="req-index">01</div><div><div class="card-heading">等级与动态过滤</div>
                <p>DEBUG / INFO / WARN / ERROR；每个进程可独立调整阈值。</p></div>
            </div>
            <div class="deck-card requirement-card">
                <div class="req-index">02</div><div><div class="card-heading">日志字段与顺序</div>
                <p><code>[毫秒时间戳][等级][PID][模块][正文]</code></p></div>
            </div>
            <div class="deck-card requirement-card">
                <div class="req-index">03</div><div><div class="card-heading">两类输出</div>
                <p>控制台输出 + 按大小轮转的文件输出；远程收集不是首版要求。</p></div>
            </div>
            <div class="deck-card requirement-card">
                <div class="req-index">04</div><div><div class="card-heading">多进程汇聚与可靠边界</div>
                <p>FDBus 上报、有限窗口排序、有界异步队列、溢出计数、正常退出排空。</p></div>
            </div>
        </div>
        <div class="callout callout-blue"><strong>实现映射：</strong><code>Logger</code> → <code>FdbusLogTarget</code> → <code>LogServer</code> → <code>LogAggregator</code> → 两个 Sink。</div>
        <div class="anchorline">字段定义与能力边界：<code>include/ipc_log/log_record.h</code> · <code>proto/log_protocol.proto</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>03</div></div>
</div>

<!-- SLIDE 4: Architecture -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">系统结构｜数据面汇聚，控制面定向</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="service-strip"><span class="service-dot"></span><strong>name_server</strong><span>服务发现与会话建立；不转发日志</span><code>svc://ipc_log_service</code></div>
        <div class="architecture">
            <div class="worker-stack">
                <div class="arch-label">业务工作进程</div>
                <div class="arch-worker"><strong>Worker A</strong><span>Logger → 本地 FIFO → 发送线程</span><small><code>Logger::log</code> · <code>FdbusLogTarget::run</code></small></div>
                <div class="arch-worker"><strong>Worker B … N</strong><span>独立等级 / 独立有界队列</span><small>每个进程各自拥有发送目标</small></div>
            </div>
            <div class="arch-link"><span>日志请求</span><b>→</b><small>FDBus + Protobuf</small></div>
            <div class="arch-server">
                <div class="arch-label">日志主进程</div>
                <div class="server-box"><strong>LogServer</strong><span>会话校验 · 序号校验 · 快速入队</span><small><code>onInvoke / handleRecord</code></small></div>
                <div class="server-box secondary"><strong>共享汇聚队列</strong><span>有界 FIFO → 有限窗口排序</span><small><code>LogAggregator</code> · <code>ReorderBuffer</code></small></div>
            </div>
            <div class="arch-link output-link"><span>输出</span><b>→</b></div>
            <div class="sink-stack">
                <div class="arch-label">统一输出</div>
                <div class="sink-box">终端<br><small>ConsoleSink</small></div>
                <div class="sink-box">轮转文件<br><small>RotatingFileSink</small></div>
            </div>
        </div>
        <div class="control-strip"><strong>控制面</strong><span>命令行 → LogServer → FDBus 定向请求 → 指定 worker 更新 / 查询等级 → 应用层应答</span><code>list · get-level · set-level</code></div>
        <div class="anchorline">图码锚点：<code>src/client/fdbus_transport.cpp</code> · <code>src/server/log_server.cpp</code> · <code>apps/log_server_main.cpp</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>04</div></div>
</div>

<!-- SLIDE 5: Log Path -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">一条日志的旅程｜从 API 到统一输出</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="journey">
            <div class="journey-step"><span class="step-no">01</span><strong>业务 API</strong><p>调用 <code>info()</code> 等接口。</p><small>模块 + 正文</small></div>
            <div class="journey-arrow">→</div>
            <div class="journey-step"><span class="step-no">02</span><strong>等级过滤</strong><p>低于当前进程阈值就结束。</p><small>原子阈值</small></div>
            <div class="journey-arrow">→</div>
            <div class="journey-step"><span class="step-no">03</span><strong>构造并入队</strong><p>补齐时间、PID、实例与序号。</p><small>tryPush · 有界 FIFO</small></div>
            <div class="journey-arrow">→</div>
            <div class="journey-step"><span class="step-no">04</span><strong>FDBus 上报</strong><p>后台线程序列化并发送请求。</p><small>Protobuf LogRecord</small></div>
            <div class="journey-arrow">→</div>
            <div class="journey-step"><span class="step-no">05</span><strong>校验与汇聚</strong><p>核对 session、实例、序号后入队。</p><small>ACK = 已接收入队</small></div>
            <div class="journey-arrow">→</div>
            <div class="journey-step"><span class="step-no">06</span><strong>排序与输出</strong><p>有限窗口排序，写入两个 Sink。</p><small>终端 + 轮转文件</small></div>
        </div>
        <div class="journey-result"><span>关键分层</span> 生产线程只做过滤、构造与有界入队；发送和输出工作由后台路径完成。</div>
        <div class="anchorline">入口：<code>src/common/logger.cpp::Logger::log</code> · <code>src/client/fdbus_transport.cpp::FdbusLogTarget::run</code> · <code>src/server/log_server.cpp::handleRecord</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>05</div></div>
</div>

<!-- SLIDE 6: Thread and Backpressure -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">线程与资源边界｜吞吐有上限，行为可计数</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid four resource-grid">
            <div class="deck-card compact-card"><div class="eyebrow">WORKER · 生产</div><div class="card-heading">业务线程</div><p>等级判断、记录快照、尝试入队；不等待网络 ACK 或磁盘 I/O。</p><div class="capacity">本地队列 <b>2048</b></div></div>
            <div class="deck-card compact-card accent-orange"><div class="eyebrow">WORKER · 发送</div><div class="card-heading">单独发送线程</div><p>FIFO 后台发送；同时最多一个未确认请求，按超时处理结果。</p><div class="capacity">请求超时 <b>2000 ms</b></div></div>
            <div class="deck-card compact-card"><div class="eyebrow">SERVER · 接收</div><div class="card-heading">FDBus 回调</div><p>消息长度 / 协议 / 身份校验，快速提交共享汇聚队列。</p><div class="capacity">汇聚队列 <b>8192</b></div></div>
            <div class="deck-card compact-card accent-orange"><div class="eyebrow">SERVER · 输出</div><div class="card-heading">聚合线程</div><p>消费队列、推进排序窗口，再写终端和文件 Sink。</p><div class="capacity">排序缓存 <b>4096</b></div></div>
        </div>
        <div class="split-note">
            <div><strong>队列满：</strong>拒收新记录并增加对应丢弃统计；不阻塞生产者等待容量。</div>
            <div><strong>重要边界：</strong>队列入口仍有短暂互斥锁；“异步”表示不等待网络与磁盘，不代表零开销或绝对无阻塞。</div>
        </div>
        <div class="anchorline">容量默认值：<code>include/ipc_log/fdbus_transport.h</code> · <code>include/ipc_log/log_aggregator.h</code> · <code>include/ipc_log/reorder_buffer.h</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>06</div></div>
</div>

<!-- SLIDE 7: FDBus -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">FDBus 通信｜服务发现、请求应答与 ACK 语义</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="sequence">
            <div><span>1</span><strong>服务注册 / 发现</strong><p>worker 通过服务名找到 log_server。</p></div>
            <div><span>2</span><strong>构造消息</strong><p>LogRecord 用 Protobuf 编码。</p></div>
            <div><span>3</span><strong>发送请求</strong><p>后台线程 invoke；等待请求结果。</p></div>
            <div><span>4</span><strong>回调校验</strong><p>服务端核对消息与会话身份。</p></div>
            <div><span>5</span><strong>应用层应答</strong><p>确认是否进入汇聚队列。</p></div>
        </div>
        <div class="card-grid two fdbus-lower">
            <div class="deck-card">
                <div class="card-heading">断连与失败怎么理解</div>
                <ul class="compact-list">
                    <li>发送超时会计入 uncertain：请求是否已被处理可能不确定。</li>
                    <li>未确认日志不自动重发，避免未经设计的重复和乱序。</li>
                    <li>ACK 只表示服务端接受并入队，不代表已写出，更不代表 fsync。</li>
                </ul>
            </div>
            <div class="deck-card accent-blue">
                <div class="card-heading">本地与跨机边界</div>
                <p>FDBus 支持不同传输地址；本项目当前演示目标是同机多进程。</p>
                <div class="tag-row"><span class="tag">ipc:// 本地 IPC</span><span class="tag">tcp:// 网络地址</span></div>
                <div class="callout">跨机延迟、重连、部署与安全运维：作为方案分析，不声称已验证。</div>
            </div>
        </div>
        <div class="anchorline">图码锚点：<code>src/client/fdbus_transport.cpp</code> · <code>src/server/log_server.cpp::onInvoke</code> · <code>proto/log_protocol.proto</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>07</div></div>
</div>

<!-- SLIDE 8: Dynamic Level Control -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">独立动态调级｜按进程实例定向控制</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two control-layout">
            <div class="terminal-card">
                <div class="terminal-head"><span></span><span></span><span></span><b>log_server 控制台</b></div>
                <pre><code>list
get-level &lt;process_instance&gt;
set-level &lt;process_instance&gt; WARN</code></pre>
                <p>先用 <code>list</code> 获取在线实例，再替换占位符。等级：DEBUG / INFO / WARN / ERROR。</p>
            </div>
            <div class="control-steps">
                <div class="control-step"><b>01</b><span><strong>识别目标</strong><br>process_instance 区分重启与 PID 复用。</span></div>
                <div class="control-step"><b>02</b><span><strong>定向请求</strong><br>LogServer 按当前 FDBus session 解析 worker。</span></div>
                <div class="control-step"><b>03</b><span><strong>原子更新阈值</strong><br>目标 Logger 的后续调用使用新等级。</span></div>
                <div class="control-step"><b>04</b><span><strong>核对应用层回复</strong><br>超时可能是结果不确定，不能简单当作“未生效”。</span></div>
            </div>
        </div>
        <div class="callout callout-blue">独立调级只影响之后的日志过滤；已经进入队列的记录继续走原有发送和输出链路。</div>
        <div class="anchorline">图码锚点：<code>src/server/log_server.cpp::resolveSession / setWorkerLevel</code> · <code>src/client/fdbus_transport.cpp::onControlInvoke</code> · <code>src/common/logger.cpp::setLevel</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>08</div></div>
</div>

<!-- SLIDE 9: Ordering and Sinks -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">有限窗口排序｜统一格式，双路输出</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two order-layout">
            <div class="deck-card">
                <div class="eyebrow">REORDER BUFFER</div><div class="card-heading">水位与释放条件</div>
                <div class="metric-grid">
                    <div class="metric"><b>50 ms</b><span>排序窗口</span></div>
                    <div class="metric"><b>100 ms</b><span>输入空闲释放</span></div>
                    <div class="metric"><b>4096</b><span>缓存容量</span></div>
                </div>
                <p>按最大已见时间戳推进水位；空闲或容量边界促使缓存收敛。迟到记录仍会输出并计数。</p>
                <div class="callout">保证的是有限窗口排序，不是跨进程全局严格顺序，也不是固定输出时限。</div>
            </div>
            <div class="deck-card accent-orange">
                <div class="eyebrow">FORMATTING + SINKS</div><div class="card-heading">题目规定的显示顺序</div>
                <div class="log-sample">[2026-08-12 10:30:45.123][WARN][PID:12345][NetManager]<br>Connection established to 192.168.1.1:8080</div>
                <div class="sink-pair"><div><strong>stdout / stderr</strong><small>ConsoleSink</small></div><div><strong>按大小轮转</strong><small>RotatingFileSink</small></div></div>
            </div>
        </div>
        <div class="anchorline">排序：<code>src/server/reorder_buffer.cpp::releaseReady</code> · 格式：<code>src/sinks/log_formatter.cpp</code> · 输出：<code>src/sinks/console_sink.cpp</code> / <code>src/sinks/rotating_file_sink.cpp</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>09</div></div>
</div>

<!-- SLIDE 10: Shutdown -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">正常退出｜系统内部逐层排空</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="shutdown-flow">
            <div class="shutdown-step"><span>01</span><strong>停止生产</strong><p>worker 业务不再提交新日志</p></div><div class="shutdown-arrow">→</div>
            <div class="shutdown-step"><span>02</span><strong>排空 worker 队列</strong><p>关闭入口，发送已入队记录</p></div><div class="shutdown-arrow">→</div>
            <div class="shutdown-step"><span>03</span><strong>停止服务端接收</strong><p>LogServer 结束新请求与控制回调</p></div><div class="shutdown-arrow">→</div>
            <div class="shutdown-step"><span>04</span><strong>排空并刷新 Sink</strong><p>汇聚队列、排序缓存、两个输出流</p></div>
        </div>
        <div class="card-grid two shutdown-notes">
            <div class="boundary-card"><strong>已移除业务触发的跨进程 FlushRequest</strong><p>排空责任留在 worker / log_server 的生命周期关闭流程内；业务代码不需要发起 flush 控制命令。</p></div>
            <div class="boundary-card warning"><strong>flush 不等于掉电安全</strong><p>只覆盖已进入相应队列的记录；传输中、进程崩溃或强制终止仍可能丢失。流刷新不等于 fsync。</p></div>
        </div>
        <div class="anchorline">图码锚点：<code>src/client/fdbus_transport.cpp::shutdown</code> · <code>src/server/log_server.cpp::shutdown</code> · <code>src/server/log_aggregator.cpp::shutdown</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>10</div></div>
</div>

<!-- SLIDE 11: Tradeoffs -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">方案权衡｜选择背后的代价与适用条件</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="compare-table">
            <div class="compare-row compare-head"><span>问题</span><span>当前选择</span><span>为什么 / 代价</span></div>
            <div class="compare-row"><span>同步直发 vs 异步队列</span><span class="choice">有界异步队列</span><span>隔离网络与磁盘等待；增加线程、队列和退出管理。</span></div>
            <div class="compare-row"><span>满载：阻塞 / 扩容 / 丢弃</span><span class="choice">拒收新项并计数</span><span>限制延迟和内存；过载时接受有统计的日志损失。</span></div>
            <div class="compare-row"><span>FDBus client/server vs pub/sub</span><span class="choice">client/server</span><span>请求应答和定向控制清晰；集中服务端会形成汇聚热点。</span></div>
            <div class="compare-row"><span>Protobuf vs 自定义协议</span><span class="choice">Protobuf</span><span>字段演进和跨端解析更明确；需要生成代码与运行依赖。</span></div>
            <div class="compare-row"><span>本地输出 vs 远程收集</span><span class="choice">终端 + 本地轮转文件</span><span>部署简单；跨机收集需另评估网络、重试、安全和运维成本。</span></div>
        </div>
        <div class="callout callout-blue">性能结论需要真实压测支撑；当前材料不宣称未经测量的吞吐或延迟优势。</div>
        <div class="anchorline">对比维度对应题目 A 级要求：延迟、可靠性、复杂度、可维护性、运维与实现成本。</div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>11</div></div>
</div>

<!-- SLIDE 12: AI Collaboration -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">AI 协作实践｜人主导需求边界与最终方案</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="ai-timeline">
            <div class="ai-stage"><div class="stage-label">题目目标</div><div class="stage-card"><strong>正常退出时排空</strong><p>要求关注已入队日志的收尾，不等同于开放一个业务可调用的跨进程 flush 协议。</p></div></div>
            <div class="ai-connector">→</div>
            <div class="ai-stage"><div class="stage-label">人工评审</div><div class="stage-card orange"><strong>核对复杂度与责任边界</strong><p>曾讨论过 FlushRequest / 待处理请求状态。人工确认它扩大了协议与竞态面，超出了题目目标。</p></div></div>
            <div class="ai-connector">→</div>
            <div class="ai-stage"><div class="stage-label">修正后</div><div class="stage-card"><strong>收敛到生命周期排空</strong><p>worker 关闭本地发送队列；log_server 关闭接收后排空汇聚队列并刷新 Sink。</p></div></div>
        </div>
        <div class="ai-review">
            <div><strong>AI 的边界</strong><p>协助梳理调用链、比较方案、生成修改候选；不能替代题目解释、源码核对和验证。</p></div>
            <div><strong>人工把关与证据</strong><p>用户决定删除跨进程 flush 协议。2026-10-28 静态核对已完成；Ubuntu 构建与退出演示仍待执行。</p></div>
        </div>
        <div class="anchorline">人工校验点：<code>proto/log_protocol.proto</code> · <code>FdbusLogTarget::shutdown</code> · <code>LogAggregator::shutdown</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>12</div></div>
</div>

<!-- SLIDE 13: Validation -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">问题与验收状态｜把“已实现”和“已验证”分开</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="status-grid">
            <div class="status-card"><span class="status-label pending">待修复</span><strong>F1–F5 审查项</strong><p>按主计划优先级继续修复；最新审查仍记录关闭竞态等问题。</p></div>
            <div class="status-card"><span class="status-label changed">已改，待运行</span><strong>F6–F8 相关修改</strong><p>改动已完成，但尚未在 Ubuntu 验证；flush 相关实现按新设计取代。</p></div>
            <div class="status-card"><span class="status-label pending">待实机</span><strong>当前源码 Linux 回归</strong><p>完整构建、双 worker、独立调级、队列边界、断连和退出路径未完成本轮验收。</p></div>
        </div>
        <div class="acceptance-note">
            <div class="acceptance-mark">!</div>
            <div><strong>AT1–AT5 的具体定义在当前已读材料中缺失</strong><p>不自行编造测试含义或通过结论。答辩前应先取得题目配套验收表，再逐项补充运行证据。</p></div>
        </div>
        <div class="anchorline">状态来源：<code>AGENTS.md</code> · <code>docs/implementation-plan.md</code> · <code>docs/code-review-2026-10-27.md</code></div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>13</div></div>
</div>

<!-- SLIDE 14: Reflection and Suggestions -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">收获、反思与建议</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two reflection-grid">
            <div class="deck-card">
                <div class="eyebrow">收获与反思</div><div class="card-heading">从功能点转向系统边界</div>
                <ul class="compact-list">
                    <li>并发系统要同时解释线程、队列、超时、失败和退出顺序。</li>
                    <li>ACK、写入、flush、fsync 是不同阶段，不能把接收应答说成持久化完成。</li>
                    <li>图纸、文档、源码和运行证据必须互相对应；静态核对不能代替目标 Linux 验收。</li>
                </ul>
            </div>
            <div class="deck-card accent-orange">
                <div class="eyebrow">建设性建议</div><div class="card-heading">让交付标准更容易落地</div>
                <ul class="compact-list">
                    <li>题目附上 AT1–AT5 的明确步骤、预期输出和通过判据。</li>
                    <li>预留稳定 Ubuntu / FDBus 环境和至少一次完整联调时间。</li>
                    <li>按阶段安排设计评审，让需求边界、实现范围和验收证据尽早对齐。</li>
                </ul>
            </div>
        </div>
        <div class="callout">本项目当前最重要的下一步：先处理代码审查项，再基于稳定源码完成 Linux 专项回归和正式图码材料。</div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>14</div></div>
</div>

<!-- SLIDE 15: Demo and Q&A -->
<div class="slide" data-layout="default">
    <div class="slide-header">
        <div class="slide-title" contenteditable="true">演示路径与问答</div>
        <div class="slide-divider"></div>
    </div>
    <div class="slide-body">
        <div class="card-grid two demo-layout">
            <div class="deck-card">
                <div class="eyebrow">UBUNTU DEMO · README 命令</div><div class="card-heading">构建并启动双 worker</div>
                <pre class="command-block"><code>cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ipc_log_system --parallel
bash scripts/run_demo.sh build logs 100</code></pre>
                <p>集中调级：<code>bash scripts/run_control_demo.sh build control-logs 120</code></p>
                <div class="pending-banner">以上是 README 中的演示入口；当前源码尚待 Ubuntu 实际运行验收。</div>
            </div>
            <div class="demo-agenda">
                <div><b>01</b><span>看两个 worker 的 PID / 实例与统一日志输出</span></div>
                <div><b>02</b><span>演示 list、get-level、set-level 的独立调级</span></div>
                <div><b>03</b><span>说明排序边界、过载计数与正常退出排空</span></div>
                <div class="qa"><strong>Q &amp; A</strong><span>欢迎提问与指正</span></div>
            </div>
        </div>
        <div class="anchorline">演示命令来源：<code>README.md</code> · 操作前先完成当前版本构建与运行验收。</div>
    </div>
    <div class="slide-footer"><div>HJM-fast · IPC 日志系统</div><div>15</div></div>
</div>
"""

EXTRA_CSS = r"""
        :root {
            --header-font: "Microsoft YaHei", "Noto Sans SC", sans-serif;
            --body-font: "Microsoft YaHei", "Noto Sans SC", sans-serif;
        }
        .slide-cover::before {
            content: ""; position: absolute; right: 64px; top: 78px; width: 360px; height: 360px;
            border-radius: 50%; border: 1px solid rgba(68,114,196,.16);
            box-shadow: 0 0 0 34px rgba(68,114,196,.035), 0 0 0 72px rgba(237,125,49,.035);
            pointer-events: none;
        }
        .slide-cover .cover-title { position: relative; z-index: 1; font-size: 4rem; }
        .slide-cover .cover-subtitle { position: relative; z-index: 1; margin-bottom: 42px; }
        .slide-cover .cover-meta { position: relative; z-index: 1; gap: 8px; }
        .cover-stamp {
            position: absolute; right: 95px; bottom: 70px; padding: 9px 14px; border-radius: 999px;
            background: rgba(68,114,196,.08); color: var(--primary-color); font-size: .82rem;
            letter-spacing: .04em;
        }
        .cover-stamp span { color: var(--orange-color); margin-left: 8px; font-weight: 700; }
        .slide-body { font-size: 1.02rem; line-height: 1.48; }
        .slide-title { font-size: 2rem; }
        .compact-list { padding-left: 20px; }
        .compact-list li { margin-bottom: 8px; line-height: 1.45; font-size: .96rem; }
        .card-grid { display: grid; gap: 18px; height: calc(100% - 30px); }
        .card-grid.two { grid-template-columns: 1fr 1fr; }
        .card-grid.four { grid-template-columns: repeat(4, 1fr); }
        .deck-card {
            background: var(--slide-bg); border: 1px solid var(--border-color); border-radius: 12px;
            padding: 20px 22px; box-shadow: 0 5px 16px rgba(15,23,42,.045); min-width: 0;
        }
        .accent-blue { border-top: 4px solid var(--primary-color); }
        .accent-orange { border-top: 4px solid var(--orange-color); }
        .eyebrow { color: var(--primary-color); font-size: .75rem; font-weight: 700; letter-spacing: .12em; margin-bottom: 8px; }
        .card-heading { font-size: 1.18rem; font-weight: 700; color: var(--text-color); margin-bottom: 10px; }
        .deck-card p { margin: 6px 0; font-size: .93rem; line-height: 1.48; }
        .deck-card code, .anchorline code, .service-strip code, .control-strip code {
            font-family: Consolas, "Microsoft YaHei", monospace; color: var(--primary-color);
            background: rgba(68,114,196,.08); padding: 2px 5px; border-radius: 4px; font-size: .88em;
        }
        .tag-row { display: flex; flex-wrap: wrap; gap: 8px; margin: 12px 0; }
        .tag { padding: 5px 10px; border-radius: 99px; background: rgba(68,114,196,.09); color: var(--primary-color); font-size: .8rem; font-weight: 600; }
        .callout { margin-top: 13px; padding: 10px 14px; border-radius: 8px; background: rgba(237,125,49,.1); border-left: 4px solid var(--orange-color); font-size: .88rem; line-height: 1.45; }
        .callout-blue { background: rgba(68,114,196,.08); border-left-color: var(--primary-color); }
        .anchorline { margin-top: 10px; color: #64748B; font-size: .72rem; line-height: 1.3; }
        .mini-flow { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin: 20px 0; }
        .mini-flow span { background: rgba(68,114,196,.1); color: var(--primary-color); padding: 8px 10px; border-radius: 7px; font-weight: 600; font-size: .82rem; }
        .mini-flow b { color: var(--orange-color); font-size: .75rem; }
        .requirements-grid { height: calc(100% - 80px); }
        .requirement-card { display: flex; align-items: flex-start; gap: 14px; padding: 16px 18px; }
        .requirement-card p { margin: 0; font-size: .91rem; }
        .req-index { flex: 0 0 36px; height: 36px; display: grid; place-items: center; color: white; background: var(--primary-color); border-radius: 10px; font-weight: 700; }
        .service-strip, .control-strip { display: flex; align-items: center; gap: 12px; border-radius: 8px; padding: 9px 14px; background: rgba(68,114,196,.07); font-size: .82rem; margin-bottom: 12px; }
        .service-strip code, .control-strip code { margin-left: auto; }
        .service-dot { width: 9px; height: 9px; border-radius: 50%; background: var(--orange-color); }
        .architecture { display: grid; grid-template-columns: 1.15fr .28fr 1.15fr .22fr .64fr; gap: 8px; align-items: stretch; min-height: 285px; }
        .worker-stack, .arch-server, .sink-stack { display: flex; flex-direction: column; gap: 9px; }
        .arch-label { color: #64748B; font-size: .75rem; font-weight: 700; letter-spacing: .04em; }
        .arch-worker, .server-box, .sink-box { border: 1px solid var(--border-color); border-radius: 9px; background: var(--slide-bg); padding: 12px 14px; min-width: 0; }
        .arch-worker { display: flex; flex-direction: column; gap: 5px; flex: 1; }
        .arch-worker strong, .server-box strong { color: var(--primary-color); font-size: .98rem; }
        .arch-worker span, .server-box span { font-size: .78rem; }
        .arch-worker small, .server-box small, .sink-box small { color: #64748B; font-size: .68rem; }
        .server-box { flex: 1; display: flex; flex-direction: column; justify-content: center; gap: 6px; border-left: 4px solid var(--primary-color); }
        .server-box.secondary { border-left-color: var(--orange-color); }
        .arch-link { display: flex; flex-direction: column; align-items: center; justify-content: center; color: var(--orange-color); text-align: center; font-size: .7rem; }
        .arch-link b { font-size: 1.8rem; line-height: 1.3; }
        .sink-box { text-align: center; font-weight: 700; color: var(--text-color); font-size: .86rem; }
        .control-strip { margin-top: 10px; margin-bottom: 0; background: rgba(237,125,49,.09); }
        .journey { display: flex; align-items: stretch; gap: 5px; height: 290px; }
        .journey-step { flex: 1; border: 1px solid var(--border-color); border-radius: 9px; background: var(--slide-bg); padding: 15px 12px; display: flex; flex-direction: column; gap: 9px; min-width: 0; }
        .journey-step:nth-of-type(odd) { border-top: 4px solid var(--primary-color); }
        .journey-step strong { font-size: .89rem; }
        .journey-step p { font-size: .78rem; line-height: 1.4; margin: 0; }
        .journey-step small { color: #64748B; font-size: .69rem; margin-top: auto; }
        .step-no { width: 27px; height: 27px; display: grid; place-items: center; background: rgba(68,114,196,.1); color: var(--primary-color); border-radius: 50%; font-weight: 700; font-size: .72rem; }
        .journey-arrow { display: flex; align-items: center; color: var(--orange-color); font-size: 1.1rem; }
        .journey-result { margin-top: 15px; background: rgba(68,114,196,.07); padding: 12px 15px; border-radius: 8px; font-size: .9rem; }
        .journey-result span { font-weight: 700; color: var(--primary-color); margin-right: 8px; }
        .resource-grid { height: 300px; gap: 12px; }
        .compact-card { padding: 17px 15px; }
        .compact-card .eyebrow { font-size: .66rem; }
        .compact-card .card-heading { font-size: .98rem; }
        .compact-card p { font-size: .79rem; line-height: 1.45; }
        .capacity { border-top: 1px solid var(--border-color); padding-top: 10px; margin-top: auto; font-size: .74rem; color: #64748B; }
        .capacity b { color: var(--orange-color); font-size: 1.05rem; margin-left: 5px; }
        .split-note { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 14px; }
        .split-note > div { padding: 11px 14px; border-radius: 8px; background: rgba(15,23,42,.04); font-size: .83rem; line-height: 1.4; }
        [data-theme="dark"] .split-note > div { background: rgba(255,255,255,.06); }
        .sequence { display: grid; grid-template-columns: repeat(5, 1fr); gap: 12px; }
        .sequence > div { position: relative; background: var(--slide-bg); border: 1px solid var(--border-color); border-radius: 9px; padding: 14px 12px; min-height: 112px; }
        .sequence > div > span { display: inline-grid; width: 24px; height: 24px; place-items: center; border-radius: 50%; color: white; background: var(--primary-color); font-size: .74rem; font-weight: 700; margin-bottom: 6px; }
        .sequence strong { display: block; font-size: .83rem; }
        .sequence p { margin: 4px 0 0; font-size: .73rem; line-height: 1.36; color: #64748B; }
        .fdbus-lower { height: 230px; margin-top: 14px; }
        .terminal-card { background: #172033; color: #F8FAFC; border-radius: 12px; padding: 20px; }
        .terminal-head { display: flex; align-items: center; gap: 6px; color: #CBD5E1; font-size: .76rem; margin-bottom: 17px; }
        .terminal-head span { width: 8px; height: 8px; border-radius: 50%; background: #FB7185; }
        .terminal-head span:nth-child(2) { background: #FBBF24; }
        .terminal-head span:nth-child(3) { background: #34D399; margin-right: 5px; }
        .terminal-card pre { color: #A7F3D0; font-size: 1rem; line-height: 1.8; white-space: pre-wrap; }
        .terminal-card p { margin-top: 14px; color: #CBD5E1; font-size: .78rem; }
        .control-layout { height: 350px; }
        .control-steps { display: flex; flex-direction: column; justify-content: space-between; gap: 8px; }
        .control-step { display: flex; align-items: flex-start; gap: 12px; padding: 10px 12px; background: rgba(68,114,196,.06); border-radius: 8px; font-size: .79rem; line-height: 1.4; }
        .control-step b { color: var(--orange-color); font-size: .8rem; }
        .control-step strong { font-size: .86rem; }
        .order-layout { height: 390px; }
        .metric-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin: 14px 0; }
        .metric { display: flex; flex-direction: column; padding: 12px 8px; border-radius: 8px; background: rgba(68,114,196,.08); }
        .metric b { font-size: 1.25rem; color: var(--primary-color); }
        .metric span { font-size: .7rem; color: #64748B; margin-top: 3px; }
        .log-sample { padding: 16px; border-radius: 8px; background: rgba(15,23,42,.06); font-family: Consolas, monospace; font-size: .84rem; line-height: 1.7; overflow-wrap: anywhere; }
        [data-theme="dark"] .log-sample { background: rgba(255,255,255,.07); }
        .sink-pair { display: flex; gap: 10px; margin-top: 16px; }
        .sink-pair > div { flex: 1; border: 1px solid var(--border-color); border-radius: 8px; padding: 11px; display: flex; flex-direction: column; gap: 4px; }
        .sink-pair strong { font-size: .83rem; }
        .sink-pair small { color: #64748B; font-size: .72rem; }
        .shutdown-flow { display: flex; gap: 7px; align-items: stretch; min-height: 175px; }
        .shutdown-step { flex: 1; padding: 15px 12px; border-radius: 9px; background: var(--slide-bg); border: 1px solid var(--border-color); display: flex; flex-direction: column; gap: 8px; }
        .shutdown-step span { color: var(--orange-color); font-size: .8rem; font-weight: 700; }
        .shutdown-step strong { font-size: .86rem; }
        .shutdown-step p { font-size: .74rem; line-height: 1.4; color: #64748B; margin: 0; }
        .shutdown-arrow { color: var(--primary-color); display: grid; place-items: center; font-size: 1.25rem; }
        .shutdown-notes { margin-top: 16px; height: 160px; }
        .boundary-card { border-left: 4px solid var(--primary-color); background: rgba(68,114,196,.07); border-radius: 8px; padding: 15px 18px; }
        .boundary-card.warning { border-left-color: var(--orange-color); background: rgba(237,125,49,.08); }
        .boundary-card strong { font-size: .92rem; }
        .boundary-card p { font-size: .82rem; line-height: 1.45; }
        .compare-table { border: 1px solid var(--border-color); border-radius: 9px; overflow: hidden; font-size: .76rem; }
        .compare-row { display: grid; grid-template-columns: 1.25fr 1fr 1.85fr; }
        .compare-row > span { padding: 10px 12px; border-bottom: 1px solid var(--border-color); line-height: 1.35; }
        .compare-row:last-child > span { border-bottom: 0; }
        .compare-row > span + span { border-left: 1px solid var(--border-color); }
        .compare-head { background: rgba(68,114,196,.1); font-weight: 700; color: var(--primary-color); }
        .choice { font-weight: 700; color: var(--primary-color); }
        .ai-timeline { display: flex; align-items: stretch; gap: 10px; min-height: 265px; }
        .ai-stage { flex: 1; display: flex; flex-direction: column; gap: 8px; }
        .stage-label { color: #64748B; font-size: .75rem; font-weight: 700; }
        .stage-card { height: 100%; padding: 17px; border-radius: 10px; border: 1px solid var(--border-color); background: var(--slide-bg); border-top: 4px solid var(--primary-color); }
        .stage-card.orange { border-top-color: var(--orange-color); }
        .stage-card strong { font-size: .92rem; }
        .stage-card p { font-size: .82rem; line-height: 1.48; margin-top: 10px; }
        .ai-connector { display: grid; place-items: center; color: var(--orange-color); font-size: 1.3rem; padding-top: 28px; }
        .ai-review { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 14px; }
        .ai-review > div { background: rgba(68,114,196,.06); border-radius: 8px; padding: 12px 14px; }
        .ai-review strong { color: var(--primary-color); font-size: .82rem; }
        .ai-review p { margin: 4px 0 0; font-size: .76rem; line-height: 1.4; }
        .status-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 13px; }
        .status-card { position: relative; padding: 16px; border: 1px solid var(--border-color); border-radius: 9px; background: var(--slide-bg); min-height: 180px; }
        .status-label { display: inline-block; padding: 4px 8px; border-radius: 99px; font-size: .67rem; font-weight: 700; margin-bottom: 10px; }
        .status-label.pending { color: #A33B2B; background: #FDE8E4; }
        .status-label.changed { color: #815500; background: #FFF3CD; }
        .status-card strong { display: block; font-size: .9rem; }
        .status-card p { font-size: .76rem; line-height: 1.45; color: #64748B; }
        .acceptance-note { display: flex; gap: 14px; align-items: center; margin-top: 16px; padding: 13px 16px; background: rgba(237,125,49,.1); border-radius: 9px; }
        .acceptance-mark { width: 30px; height: 30px; display: grid; place-items: center; flex: 0 0 30px; border-radius: 50%; color: white; background: var(--orange-color); font-weight: 700; }
        .acceptance-note strong { font-size: .84rem; }
        .acceptance-note p { margin: 4px 0 0; font-size: .75rem; line-height: 1.4; }
        .reflection-grid { height: 380px; }
        .reflection-grid .deck-card { padding: 22px 24px; }
        .demo-layout { height: 390px; }
        .command-block { background: #172033; color: #A7F3D0; padding: 14px; border-radius: 8px; font-size: .73rem; line-height: 1.6; white-space: pre-wrap; overflow-wrap: anywhere; }
        .pending-banner { margin-top: 12px; padding: 9px 11px; border-radius: 7px; background: #FFF3CD; color: #765500; font-size: .73rem; font-weight: 600; }
        .demo-agenda { display: flex; flex-direction: column; justify-content: space-between; gap: 8px; }
        .demo-agenda > div { display: flex; gap: 12px; align-items: center; padding: 11px 13px; border-radius: 8px; background: rgba(68,114,196,.06); font-size: .81rem; }
        .demo-agenda b { color: var(--orange-color); }
        .demo-agenda .qa { flex-direction: column; align-items: flex-start; flex: 1; justify-content: center; background: var(--primary-color); color: white; }
        .demo-agenda .qa strong { font-size: 1.75rem; }
        .demo-agenda .qa span { opacity: .85; font-size: .85rem; }
        [data-theme="dark"] .status-label.pending { color: #fecaca; background: rgba(185,28,28,.24); }
        [data-theme="dark"] .status-label.changed { color: #fde68a; background: rgba(161,98,7,.24); }
        [data-theme="dark"] .pending-banner { color: #fde68a; background: rgba(161,98,7,.24); }
        @media (max-width: 900px) {
            .topbar { padding: 0 10px; } .brand span { display: none; } .controls-group { gap: 5px; }
            .controls-group .btn { padding: 7px 9px; font-size: .75rem; }
            .presentation-area { padding: 10px; } .bottom-toolbar { padding: 0 10px; }
        }
"""


class DeckAudit(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.slide_count = 0
        self.active_slide_count = 0
        self.ids: set[str] = set()
        self.duplicate_ids: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        classes = (values.get("class") or "").split()
        if tag == "div" and "slide" in classes:
            self.slide_count += 1
            if "active" in classes:
                self.active_slide_count += 1
        element_id = values.get("id")
        if element_id:
            if element_id in self.ids:
                self.duplicate_ids.add(element_id)
            self.ids.add(element_id)


def replace_once(text: str, pattern: str, replacement: str, flags: int = 0) -> str:
    updated, count = re.subn(pattern, replacement, text, count=1, flags=flags)
    if count != 1:
        raise RuntimeError(f"Expected one match for pattern: {pattern!r}; found {count}")
    return updated


def main() -> None:
    source = TEMPLATE.read_text(encoding="utf-8")
    html = replace_once(source, r"<title>.*?</title>", "<title>IPC 日志系统答辩｜HJM-fast</title>")
    html = re.sub(r'\s*<link href="https://fonts\.googleapis\.com/[^"]+"\s+rel="stylesheet">', "", html, count=1)
    html = replace_once(
        html,
        r'<div class="brand">.*?</div>',
        '<div class="brand">IPC 日志系统 <span>| HJM-fast · 答辩演示</span></div>',
        flags=re.S,
    )
    html = replace_once(html, "广州视源电子科技股份有限公司", "HJM-fast · IPC 日志系统")
    html = html.replace("cvte_intern_slide_current", "hjm_log_defense_slide_current")
    html = html.replace("cvte_intern_slide_theme", "hjm_log_defense_theme")
    html = html.replace("cvte_intern_slide_content", "hjm_log_defense_content")
    html = html.replace("六环实习总结汇报_网页版.html", "IPC日志系统答辩_网页版.html")
    html = html.replace("Slide 1 / 13", "Slide 1 / 15")
    html = replace_once(
        html,
        r'(<div class="slide-viewport" id="slide-viewport">).*?(</div>\s*</div>\s*</div>\s*<!-- Bottom Toolbar -->)',
        lambda m: m.group(1) + "\n" + SLIDES_HTML + "\n" + m.group(2),
        flags=re.S,
    )
    html = replace_once(html, "</style>", EXTRA_CSS + "\n    </style>")
    html = replace_once(
        html,
        "    // State Management\n    let currentSlide = 0;",
        """    // Restore locally saved slide edits before initializing navigation.
    const savedDeck = localStorage.getItem('hjm_log_defense_content');
    if (savedDeck) {
        const savedDocument = new DOMParser().parseFromString(savedDeck, 'text/html');
        const savedSlides = savedDocument.body.innerHTML;
        if (savedSlides.trim()) document.getElementById('slide-viewport').innerHTML = savedSlides;
    }

    // State Management
    let currentSlide = 0;""",
    )
    html = replace_once(
        html,
        r"document\.getElementById\('btn-save'\)\.addEventListener\('click', \(\) => \{.*?\n    \}\);",
        """document.getElementById('btn-save').addEventListener('click', () => {
        localStorage.setItem('hjm_log_defense_content', viewport.innerHTML);
        alert('已保存修改；下次打开此浏览器会自动恢复。');
    });""",
        flags=re.S,
    )
    html = html.replace("                保存本地", "                本地保存")
    audit = DeckAudit()
    audit.feed(html)
    audit.close()
    if audit.slide_count != 15 or audit.active_slide_count != 1 or audit.duplicate_ids:
        raise RuntimeError(
            f"Deck structure check failed: slides={audit.slide_count}, "
            f"active={audit.active_slide_count}, duplicate_ids={sorted(audit.duplicate_ids)}"
        )
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(html, encoding="utf-8")
    print(f"Generated {OUTPUT}")
    print(f"Slides: {audit.slide_count}; active slides: {audit.active_slide_count}")
    print(f"Bytes: {OUTPUT.stat().st_size}")


if __name__ == "__main__":
    main()
