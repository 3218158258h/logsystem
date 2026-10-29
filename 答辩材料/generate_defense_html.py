"""Reuse the supplied generator/template to build an offline, editable HTML defense."""
from pathlib import Path
import ast, base64, hashlib, html, json, re

ROOT=Path(__file__).resolve().parents[1]
OUTPUT=Path(__file__).with_name("IPC日志系统答辩-完整版.html")
SOURCE=ROOT/"generate_slides.py"
TEMPLATE=ROOT/"【模板】六环实习总结网页版.html"
ARCH=ROOT/"架构图.png"
UML=Path(__file__).with_name("uml-class-diagram.svg")
FILLED=Path(__file__).with_name("模板填写内容.json")
def esc(s): return html.escape(str(s),quote=True)
def para(s,cls=""): return f'<p class="{cls}" contenteditable="true">{s}</p>'
def bullets(items): return '<ul class="def-list">'+''.join(f'<li contenteditable="true">{x}</li>' for x in items)+'</ul>'
def col(title,content): return '<section class="def-col"><h3 contenteditable="true">'+title+'</h3>'+content+'</section>'
def two(left,right): return '<div class="def-two">'+left+right+'</div>'
def table(head,rows,widths=None):
    cg='' if not widths else '<colgroup>'+''.join(f'<col style="width:{w}%">' for w in widths)+'</colgroup>'
    return '<table class="def-table">'+cg+'<thead><tr>'+''.join(f'<th contenteditable="true">{x}</th>' for x in head)+'</tr></thead><tbody>'+''.join('<tr>'+''.join(f'<td contenteditable="true">{x}</td>' for x in row)+'</tr>' for row in rows)+'</tbody></table>'
def check(s): return '<div class="figure-check" contenteditable="true">人工核对：'+s+'</div>'
def code(s): return '<pre class="def-code"><code>'+esc(s)+'</code></pre>'
def svg_start(h=430):
    return f'<svg class="tech-diagram" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1120 {h}" role="img" aria-label="代码设计示意图"><defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#4472C4"/></marker><marker id="inherit" viewBox="0 0 12 12" refX="11" refY="6" markerWidth="10" markerHeight="10" orient="auto"><path d="M 1 1 L 11 6 L 1 11 z" fill="white" stroke="#4472C4"/></marker></defs>'
def svg_text(x,y,s,sz=21,weight=400,anchor="middle",color="#25354d"):
    return f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-family="Microsoft YaHei, sans-serif" font-size="{sz}" font-weight="{weight}" fill="{color}">{esc(s)}</text>'

def seq(actors,messages,bands=()):
    n=len(actors);xs=[80+i*960/(n-1) for i in range(n)]
    result=svg_start(440)
    for first,last,label in bands:
        x=xs[first]-76;w=xs[last]-xs[first]+152
        result+=f'<rect x="{x}" y="0" width="{w}" height="438" rx="8" fill="#f5f8fd"/>'
        result+=svg_text(x+w/2,19,label,15,600,color="#64748b")
    for i,a in enumerate(actors):
        x=xs[i]
        result+=f'<rect x="{x-74}" y="30" width="148" height="52" rx="5" fill="#eaf1fb" stroke="#b6c9e4"/>'
        for j,line in enumerate(a.split("|")):result+=svg_text(x,51+j*20,line,17,600)
        result+=f'<line x1="{x}" y1="85" x2="{x}" y2="430" stroke="#b5c0cf" stroke-dasharray="5 5"/>'
    for k,(a,b,label,reply) in enumerate(messages):
        y=115+k*38;x1,x2=xs[a],xs[b]
        color="#64748b" if reply else "#4472c4"
        dash='stroke-dasharray="6 4"' if reply else ""
        if a==b:
            side=1 if x1<900 else -1
            result+=f'<path d="M{x1},{y} h{27*side} v16 h{-27*side}" fill="none" stroke="{color}" stroke-width="2" marker-end="url(#arrow)"/>'
            result+=svg_text(x1+side*37,y+8,label,17,anchor="start" if side>0 else "end",color=color)
        else:
            result+=f'<line x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="{color}" stroke-width="{2 if reply else 2.6}" {dash} marker-end="url(#arrow)"/>'
            result+=svg_text((x1+x2)/2,y-8,label,17,color=color)
    return result+"</svg>"

def project_slides():
    result=[]
    def add(title,body,anchor,notes=""):
        result.append(dict(title=title,body=body,anchor=anchor,notes=notes,kind="project"))
    png="data:image/png;base64,"+base64.b64encode(ARCH.read_bytes()).decode("ascii")
    add("系统架构",
        '<button class="architecture-wrap architecture-open diagram-open" data-diagram-title="系统架构" title="点击放大架构图"><img class="architecture-image" alt="系统架构图" src="'+png+'"></button>'+
        check("按进程划分职责；name_server 负责发现，业务与 log_server 直接通信。"),
        "架构图.png；src/server/log_server.cpp · LogServer::Impl；CMakeLists.txt",
        "产品为log_server与两个库。独立示例复用产品库。每个FdbusLogTarget有本地FIFO和发送线程，服务端所有来源共享FIFO及重排缓存。")
    uml=UML.read_text(encoding="utf-8-sig")
    uml=re.sub(r'<\?xml.*?\?>',"",uml,flags=re.S).strip()
    uml=uml.replace("<svg ", '<svg class="tech-diagram uml-diagram" role="img" aria-label="核心类与接口关系" ',1)
    add("核心类与接口关系",
        '<button class="diagram-open uml-open" data-diagram-title="核心类与接口关系" title="点击放大 UML 类图">'+uml+'</button>',
        "include/ipc_log/log_target.h · ILogTarget；include/ipc_log/fdbus_transport.h · LogServer",
        "关键接口：Logger::log与ILogTarget::submit返回本地是否接受；shutdown返回ShutdownResult；LogServer::setWorkerLevel返回WorkerControlResult，confirmed需应用回复匹配；ILogSink::write/flush返回SinkResult。FdbusLogTarget弱引用Logger，避免强引用环。完整签名与线程归属见详细设计。")
    add("FDBus：发现、直连与消息调度",
        two(col("连接如何建立",bullets([
            "<b>名字服务：</b>log_server 注册服务名；客户端订阅并获得地址。",
            "<b>Session：</b>业务进程直连日志服务；日志与控制复用会话。",
            "<b>传输层：</b>本地可用 Unix 域 socket，跨机使用 TCP。"])),
            col("消息如何流转",bullets([
            "<b>Protobuf：</b>发送端编码记录，接收回调解码并校验。",
            "<b>Context / event loop：</b>调度在线、离线、请求与回复回调。",
            "<b>线程分工：</b>回调只校验、入队和应答；发送与输出各有后台线程。"]))),
        "src/client/fdbus_transport.cpp · Client / sendRequest；src/server/log_server.cpp · Server；SDK CFdEventLoop::dispatch",
        "当前端点未配置额外worker，回调在默认FDBus context执行。当前SDK事件循环使用poll。name_server不是日志代理。跨机与本地使用同一业务消息，但还需要考虑地址可达性、时钟、失败及安全策略。")
    add("日志数据：从业务调用到双路输出",
        seq(["业务线程|Logger","本地 FIFO|LogRecord","发送线程|FDBus Client","服务接收回调|LogServer","共享 FIFO|重排缓存","双路 Sink|控制台 / 文件"],[
            (0,1,"① 记录入队",False),(1,2,"② 出队",False),
            (2,3,"③ Protobuf 记录",False),(3,4,"④ 校验后入队",False),
            (3,2,"ACK：接收确认",True),(4,4,"⑤ 有限窗口重排",False),
            (4,5,"⑥ 格式化文本",False),(5,5,"⑦ 写控制台与文件",False)],
            [(0,2,"业务进程"),(3,5,"日志主进程")])+
        check("实线是记录与文本流，虚线是确认；业务调用入队后返回，ACK 仅确认服务端接收。"),
        "src/common/logger.cpp · log；src/client/fdbus_transport.cpp · run；src/server/log_server.cpp · handleRecord；src/server/log_aggregator.cpp · outputReady",
        "图按记录的数据形态展示一种正常交错，ACK不约束实际写出先后。Logger在构造LogRecord之前检查阈值；发送线程序列化，接收回调校验，唯一汇聚线程排序、格式化并顺序调用两个Sink。去掉队列会把网络或磁盘等待带入业务，不过滤则增加记录构造与IPC成本。")
    add("控制数据：指定实例动态调级",
        seq(["控制台|命令线程","LogServer|会话表","客户端 A|控制回调","Logger A|原子阈值","业务 A|后续日志"],[
            (0,1,"实例 A / ERROR",False),(1,2,"LevelControlRequest",False),
            (2,3,"requested_level",False),(3,2,"actual_level",True),
            (2,1,"Reply + request_id",True),(1,0,"confirmed",True),
            (3,4,"新记录按 ERROR 过滤",False)],
            [(0,1,"日志主进程"),(2,4,"业务进程 A")])+
        check("控制只改变 A 的阈值；B 独立运行，已入队记录继续处理。"),
        "src/server/log_server.cpp · setWorkerLevel / onControlReply；src/client/fdbus_transport.cpp · onControlInvoke；src/common/logger.cpp · setLevel",
        "控制请求在回调中执行，不走日志FIFO。服务端核对request_id、实例号和actual_level才确认；控制超时为uncertain。图中的业务A代表后续产生的记录，不是Logger主动回调业务。")
    add("有限窗口排序与统一输出",
        two(col("释放记录的条件",bullets([
            "<b>时间水位：</b>最大已见时间戳减去窗口，默认 50ms。",
            "<b>输入空闲：</b>默认 100ms 后释放积压；退出时释放剩余。",
            "<b>容量兜底：</b>超过 4096 条时提前释放最早记录。"])),
            col("统一输出",bullets([
            "按时间戳及辅助键排序；迟到记录照常输出并计数。",
            "输出前才格式化，统一写控制台与轮转文件。",
            "ERROR → stderr，其余 → stdout；文件默认 5MB、3 份备份。"])))+
        code("[2026-10-30 09:15:02.123][INFO][PID:12345][orders][order created]"),
        "src/server/reorder_buffer.cpp · releaseReady；src/sinks/log_formatter.cpp · format；src/sinks/rotating_file_sink.cpp · write",
        "timestamp_ms在协议中为epoch毫秒，文本格式化时转成本地时间并带PID:前缀。有限窗口不保证跨窗口全序。forced_releases只统计非drain时因容量超限提前释放，不计退出或空闲drain。")
    add("IPC 与远程收集方案对比",
        table(["方案","适用与收益","实现、可靠性与运维成本"],[
            ["FDBus / 本地 socket","当前采用：名字服务、会话、消息调度由 SDK 管理","业务仍负责队列、错误统计和退出；本地连接与部署较简单"],
            ["直接 socket","适合希望完全掌控协议和数据通道的场景","自行实现发现、分帧、重连与超时，维护成本更高"],
            ["共享内存","适合大批量数据交换，可减少跨进程复制","同步、缓冲区归属、满队列与崩溃恢复更复杂"],
            ["本地文件 / 远程收集","本地直接写出；远程便于多机统一检索与留存","网络增加延迟与失败点；还需缓冲、去重、鉴权加密及收集端运维"]],[23,35,42]),
        "题目 §2.3、A级方案对比；SDK CNameServer；src/client/fdbus_transport.cpp · Client",
        "本项目双路Sink为本地控制台和文件。本页远程收集为方案分析。跨机TCP还要考虑地址可达性、时钟偏差与版本兼容；ACK范围、重试去重和持久化决定可靠交付成本。")
    add("AI 协作：方案否决与缺陷纠偏",
        two(col("人主导：纠正退出方案",bullets([
            "<b>AI 方案：</b>将“退出时 flush”扩展成远程请求与队列屏障。",
            "<b>人的追问：</b>业务进程为何能提前冲刷其他来源？题目是否需要？",
            "<b>修正：</b>移除跨进程 flush；保留各端 shutdown 和内部输出收尾。"])),
            col("历史缺陷：陈旧日志判断就绪",bullets([
            "<b>位置 / 根因：</b>旧控制脚本用残留 worker-a 文本判就绪，过早发送 list。",
            "<b>修正：</b>清空旧输出，等待本轮 A/B 两个来源后再查询。",
            "<b>历史验证：</b>用户回传调级与收尾成功；accepted=307，dropped=0，write_errors=0。"]))),
        "沟通问题与AI纠偏记录.md · R3-06 / 退出语义纠偏；include/ipc_log/log_target.h；proto/log_protocol.proto",
        "人负责需求解释、方案选择和最终接受，AI辅助实现与定位。远程flush并非普遍错误，而是超出本题必要范围。陈旧日志是AI实现缺陷，其验证数据只对应当时脚本版本；后来示例工程与产品分离，旧脚本已删除。")
    return result

def compose_template_sections(source):
    from lxml import html as dom
    wanted=(1,2,3,4,5,7,9,10,11,12,13)
    pages=None
    if OUTPUT.exists():
        current=dom.fromstring(OUTPUT.read_text(encoding="utf-8"))
        nodes=current.xpath('//*[@data-template-slide]')
        if nodes:
            pages={int(n.get("data-template-slide")):dom.tostring(n,encoding="unicode") for n in nodes}
    if pages is None and FILLED.exists():
        pages={}
        for raw in json.loads(FILLED.read_text(encoding="utf-8")):
            node=dom.fromstring(raw)
            pages[int(node.get("data-template-slide"))]=raw
    if pages is None or not set(wanted).issubset(pages):
        raise RuntimeError("Missing selected filled template pages")
    FILLED.write_text(json.dumps([pages[i] for i in wanted],ensure_ascii=False,indent=2),encoding="utf-8")
    def find(node,cls):
        return node.xpath('.//*[contains(concat(" ",normalize-space(@class)," ")," '+cls+' ")]')
    def template_page(i):
        node=dom.fromstring(pages[i])
        node.set("class",node.get("class").replace(" active",""))
        heading=find(node,"slide-title") or find(node,"cover-title") or find(node,"trans-title")
        return dict(title=heading[0].text_content().strip() if heading else "",body=node,kind="template",anchor="",notes="")
    slides=[template_page(i) for i in (1,2,3,4,5)]+project_slides()+[
        template_page(7),template_page(9),template_page(10),template_page(11),template_page(12),template_page(13)]
    for index,item in enumerate(slides,1):
        if item["kind"]!="template":continue
        node=item["body"]
        if index==1:node.set("class",node.get("class")+" active")
        for footer in find(node,"slide-footer"):footer[-1].text=str(index)
        item["body"]=dom.tostring(node,encoding="unicode")
    return slides


CSS=r"""
/* Adapt the supplied master for technical evidence while retaining its branding. */
.project-slide { padding:48px 64px 32px; }
.project-slide .slide-header { margin-bottom:15px; padding-bottom:8px; }
.project-slide .slide-title { max-width:900px; font-size:2.2rem; }
.project-slide .slide-logo { top:42px; right:64px; width:184px; height:34px; }
.project-slide .slide-body { min-height:0; font-size:21px; line-height:1.55; }
.project-slide .slide-footer { flex-shrink:0; margin-top:12px; height:31px; padding-top:8px; font-size:12px; }
.project-slide .slide-footer .source-anchor { max-width:1040px; line-height:1.35; }
.def-two { display:grid; grid-template-columns:1fr 1fr; gap:38px; align-items:start; }
.def-col { min-width:0; }
.def-col h3 { color:var(--orange-color); font-size:25px; line-height:1.35; margin:2px 0 17px; font-weight:600; }
.def-list { padding-left:25px; }
.def-list li { padding-left:3px; margin-bottom:15px; }
.def-list li::marker { color:var(--orange-color); }
.def-table { width:100%; border-collapse:collapse; table-layout:fixed; font-size:19px; line-height:1.42; }
.def-table th { text-align:left; color:var(--primary-color); padding:10px 12px; border-bottom:2px solid var(--primary-color); font-weight:600; }
.def-table td { vertical-align:top; padding:11px 12px; border-bottom:1px solid var(--border-color); overflow-wrap:anywhere; }
.def-table th:first-child,.def-table td:first-child { padding-left:0; }
.def-table th:last-child,.def-table td:last-child { padding-right:0; }
.def-code { font-family:Consolas,'Microsoft YaHei',monospace; padding:15px 18px; margin:14px 0; background:rgba(68,114,196,.055); font-size:19px; line-height:1.5; white-space:pre-wrap; overflow-wrap:anywhere; }
.def-emphasis { margin:18px 0 0; color:var(--primary-color); font-weight:600; font-size:21px; }
.def-caption { margin-top:16px; font-size:18px; color:#64748b; line-height:1.45; }
.def-closing { font-size:32px; font-weight:600; color:var(--primary-color); text-align:center; margin-top:30px; }
.tech-diagram { display:block; width:100%; height:420px; background:#fff; }
.figure-check { margin-top:8px; padding-left:12px; border-left:3px solid var(--orange-color); color:var(--slide-text); font-size:17px; line-height:1.35; }
.architecture-wrap { width:100%; height:456px; text-align:center; }
.architecture-image { max-width:100%; height:100%; object-fit:contain; background:#fff; }
.diagram-open { display:block; border:0; padding:0; background:transparent; cursor:zoom-in; }
#architecture-dialog { width:min(1600px,95vw); max-width:95vw; height:94vh; max-height:94vh; padding:18px; border:1px solid #c6d0df; border-radius:10px; background:white; color:#24354b; }
#architecture-dialog::backdrop { background:rgba(12,24,44,.65); }
.architecture-dialog-bar { display:flex; align-items:center; justify-content:space-between; gap:18px; height:45px; margin-bottom:12px; font-size:20px; }
.architecture-dialog-bar > div { display:flex; align-items:center; gap:10px; }
.architecture-dialog-body { overflow:auto; width:100%; height:calc(100% - 57px); }
#architecture-zoom-image { display:block; width:100%; max-width:none; height:auto; }

.uml-open { display:block; width:100%; height:440px; }
.uml-slide { padding-left:32px; padding-right:32px; }
.uml-slide .slide-header,.uml-slide .slide-footer { margin-left:32px; width:calc(100% - 64px); }
.uml-slide .uml-open { width:100%; height:100%; }

.uml-diagram { height:100%; }
.project-background { margin:0 0 18px; font-size:18px; line-height:1.5; }
.speaker-note { display:none!important; }
#defense-notes { position:fixed; right:24px; bottom:68px; z-index:100; width:min(660px,calc(100vw - 48px)); max-height:45vh; overflow:auto; border:1px solid var(--border-color); border-radius:10px; background:var(--slide-bg); color:var(--slide-text); box-shadow:0 8px 35px #0003; padding:20px 24px; font-size:16px; line-height:1.7; }
#defense-notes[hidden] { display:none; }
#defense-notes h3 { margin-bottom:8px; color:var(--primary-color); font-size:19px; }
body.mode-presenting #defense-notes { bottom:24px; }
@media print {
 @page { size:1280px 720px; margin:0; }
 body,.workspace,.presentation-area,#slide-container,#slide-viewport { height:auto!important; overflow:visible!important; display:block!important; padding:0!important; margin:0!important; }
 body { background:white; }
 .topbar,.bottom-nav,.toast-help,#defense-notes { display:none!important; }
 #slide-container { width:1280px!important; max-width:none!important; border:0; border-radius:0; box-shadow:none; aspect-ratio:auto; }
 #slide-viewport { width:1280px!important; transform:none!important; }
 .slide { position:relative!important; width:1280px!important; height:720px!important; opacity:1!important; visibility:visible!important; break-after:page; page-break-after:always; background:white; color:#1e293b; }
 .slide:last-child { break-after:auto; page-break-after:auto; }
}
"""
def template_html():
    # Reuse the existing generator's assembly; do not run its template-overwrite block.
    parsed=ast.parse(SOURCE.read_text(encoding="utf-8-sig"),filename=str(SOURCE))
    safe=ast.Module(body=[n for n in parsed.body if isinstance(n,(ast.Import,ast.Assign))],type_ignores=[])
    ns={"__file__":str(SOURCE)}
    exec(compile(safe,str(SOURCE),"exec"),ns)
    rendered=ns["html_template"]
    supplied=TEMPLATE.read_text(encoding="utf-8-sig")
    # The supplied template remains authoritative if the user has customized it.
    return rendered if rendered.strip()==supplied.strip() else supplied
def replace_viewport(source,new_inner):
    tag=re.search(r'<div\b[^>]*\bid="slide-viewport"[^>]*>',source)
    if not tag: raise RuntimeError("Template viewport not found")
    start=tag.end(); depth=1; end=None
    for match in re.finditer(r"</?div\b[^>]*>",source[start:],re.I):
        depth += -1 if match.group().startswith("</") else 1
        if depth==0:
            end=start+match.start(); break
    if end is None: raise RuntimeError("Template viewport unbalanced")
    return source[:start]+"\n"+new_inner+"\n"+source[end:]
def main():
    before={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in [SOURCE,TEMPLATE,ARCH,UML]}
    source=template_html()
    slides=compose_template_sections(source)
    rendered=[]
    for i,item in enumerate(slides,1):
        body=item["body"].replace('id="arrow"',f'id="d{i}-arrow"').replace('url(#arrow)',f'url(#d{i}-arrow)')
        body=body.replace('id="inherit"',f'id="d{i}-inherit"').replace('url(#inherit)',f'url(#d{i}-inherit)')
        notes='<div class="speaker-note" aria-hidden="true">'+esc(item["notes"])+'</div>'
        if item["kind"]=="template":
            content=body
        else:
            content=f'<div class="slide project-slide" data-layout="default"><div class="slide-logo"></div><div class="slide-header"><div class="slide-title" contenteditable="true">{esc(item["title"])}</div><div class="slide-divider"></div></div><div class="slide-body">{body}</div><div class="slide-footer"><div class="source-anchor" contenteditable="true">代码 / 依据：{esc(item["anchor"])}</div><div>{i} / {len(slides)}</div></div>{notes}</div>'
        if item["title"]=="核心类与接口关系":
            content=content.replace('class="slide project-slide"','class="slide project-slide uml-slide"',1)
        rendered.append(content)
    source=replace_viewport(source,"\n".join(rendered))
    source=re.sub(r'<title>.*?</title>','<title>IPC 日志系统 · 实习答辩</title>',source,count=1,flags=re.S)
    source=re.sub(r'<link\b[^>]*fonts\.googleapis\.com[^>]*>','',source)
    source=source.replace("</head>",'<meta name="architecture-sha256" content="'+before[ARCH]+'"><meta name="uml-sha256" content="'+before[UML]+'"><meta name="source-template-sha256" content="'+before[TEMPLATE]+'"><style>'+CSS+'</style></head>')
    source=re.sub(r'(<div class="brand">).*?(</div>)',r'\1IPC 日志系统 <span>| 实习课题答辩 · 16:9</span>\2',source,count=1,flags=re.S)
    source=source.replace("cvte_intern_slide_","ipc_log_defense_review_")
    source=source.replace("六环实习总结汇报_网页版.html","IPC日志系统答辩.html")
    old_serial='const serialized = "<!DOCTYPE html>\\n<html lang=\\"zh-CN\\">\\n" + docClone.outerHTML + "\\n</html>";'
    source=source.replace(old_serial,'const serialized = "<!DOCTYPE html>\\n" + docClone.outerHTML;')
    marker='<div class="controls-group">'
    source=source.replace(marker,marker+'<button class="btn" id="btn-notes" title="讲解备注（N）">讲稿 (N)</button>',1)
    extra=r'''<dialog id="architecture-dialog"><div class="architecture-dialog-bar"><strong id="diagram-title">系统架构</strong><div><button class="btn" id="architecture-size">原始尺寸 / 适应窗口</button> <button class="btn" id="architecture-close">关闭（Esc）</button></div></div><div class="architecture-dialog-body"><img id="architecture-zoom-image" alt="系统架构大图"></div></dialog><aside id="defense-notes" hidden aria-live="polite"><h3>讲解备注</h3><div id="defense-notes-content"></div></aside>
<script>
(function(){

 const dialog=document.getElementById('architecture-dialog');
 const zoom=document.getElementById('architecture-zoom-image');
 document.querySelectorAll('.diagram-open').forEach(button=>button.addEventListener('click',()=>{
  const picture=button.querySelector('img'),svg=button.querySelector('svg');
  zoom.src=picture?picture.src:'data:image/svg+xml;charset=utf-8,'+encodeURIComponent(new XMLSerializer().serializeToString(svg));
  document.getElementById('diagram-title').textContent=button.dataset.diagramTitle;
  zoom.alt=button.dataset.diagramTitle;zoom.style.width='100%';dialog.showModal();
 }));
 document.getElementById('architecture-close').addEventListener('click',()=>dialog.close());
 document.getElementById('architecture-size').addEventListener('click',()=>{zoom.style.width=zoom.style.width==='100%'?'1640px':'100%';});
 dialog.addEventListener('close',()=>zoom.removeAttribute('src'));
 document.addEventListener('keydown',e=>{if(dialog.open){if(e.key==='Escape'){e.preventDefault();dialog.close();}e.stopImmediatePropagation();}},true);
 const panel=document.getElementById('defense-notes');
 const content=document.getElementById('defense-notes-content');
 function update(){const note=document.querySelector('.slide.active .speaker-note');content.textContent=note?note.textContent:'';}
 function toggle(){panel.hidden=!panel.hidden;update();}
 document.getElementById('btn-notes').addEventListener('click',toggle);
 document.addEventListener('keydown',e=>{if(e.key.toLowerCase()==='n'&&!e.target.isContentEditable&&!['INPUT','TEXTAREA','SELECT'].includes(e.target.tagName)){toggle();}});
 new MutationObserver(()=>{if(!panel.hidden)update();}).observe(document.getElementById('slide-viewport'),{subtree:true,attributes:true,attributeFilter:['class']});
})();
</script>'''
    source=source.replace("</body>",extra+"</body>")
    OUTPUT.write_text(source,encoding="utf-8")
    after={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in before}
    if before!=after: raise RuntimeError("Source template, generator or architecture asset changed")
    if len(slides)!=19: raise RuntimeError("Expected 19 slides")
    print(json.dumps({"output":str(OUTPUT),"slides":len(slides),"bytes":OUTPUT.stat().st_size,"sourceAssetsUnchanged":True,"architectureEmbedded":True,"umlEmbedded":True,"originalTemplatePages":11},ensure_ascii=False))
if __name__=="__main__": main()
