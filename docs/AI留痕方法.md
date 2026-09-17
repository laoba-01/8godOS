# AI 使用留痕 —— 方法与模板

> **这份文档给谁用**：8GodOS 是练手项目（**不是参赛作品**），用户准备的是 **2027 赛季**，参赛作品将另开仓库。
> 本文档把 8GodOS 上跑通的整套留痕方法提炼成可复用模板，**新项目直接照搬**，别重造一遍。
>
> 8GodOS 上的实际成品见 [`AI-使用记录.md`](AI-使用记录.md) 与 [`AI交流记录/`](AI交流记录/) —— 那份是**示例**，本文档是**方法**。

---

## 1. 先确认规则，不要从摘要开始

**踩过的坑**：8GodOS 的留痕初稿建立在一份**网络检索摘要**上，其中「须在 git commit / 开发文档 / 设计文档 / 答辩 PPT 中说明」这一条，**在规则原文里根本不存在**。这个不存在的要求被写进了记录、还被当成硬指标跟用户汇报过。

**做法**：

1. 拿到**当赛季**的规则原文（通知 / 技术方案 / 评审准则），确认是哪一年的版本 —— 赛季不同，规则会变。
2. 逐条抄录，**标明出处**（文档名 + 发布方 + 日期 + 章节）。
3. 原文没有的，**不许写进记录**。检索摘要里看着合理的表述，只能标注为「待核对」，不能当成要求。
4. 规则里提到的**其他文档**（如《技术方案》《评审准则进一步说明》）要一并找齐 —— 8GodOS 至今没拿到这两份，记录里明确写了这条边界。

---

## 2. 目录结构模板

```
<项目>/
├── docs/
│   ├── AI-使用记录.md          # 主记录, 入库
│   └── AI交流记录/
│       ├── README.md           # 索引, 入库
│       ├── <日期>-<会话ID>-<主题>.md   # 可读片段, 入库
│       └── 原始记录/            # 完整 .jsonl, 加进 .gitignore
```

`.gitignore` 追加：

```
# AI 交流原始记录(体积大)。可读片段在 docs/AI交流记录/*.md, 会入库。
docs/AI交流记录/原始记录/
```

**为什么原始记录不入库**：单个项目的会话记录可达 10 MB 量级，进 git 会让仓库变重；而规则要的是「**保留**」，不是「提交」。放在仓库目录内、被 ignore，既随项目走又不会丢。

---

## 3. 核心问题：怎么判断"哪些代码来自 AI"

**别用 git 作者判断。** 8GodOS 的**全部提交作者都是队员本人**，AI 产出的代码同样以队员名义提交。用 git log 看作者，会得出结论"M1~M5 全是队员手写" —— **而这是错的**。

**可靠判据：会话记录里的 `tool_use` 事件。** 这是机械、可复现的证据。

```python
#!/usr/bin/env python3
"""列出 AI 对项目文件发出的所有写操作 —— 判断"哪些文件是 AI 写的"。

用法:
    python3 scan_writes.py <会话目录> [路径关键词]

会话目录(Claude Code):
    ~/.claude/projects/<项目路径转义名>/    例: D--8GodOS
路径关键词用于过滤掉与项目无关的会话, 可省略。
"""
import json, glob, os, sys

sess_dir = sys.argv[1]
proj = sys.argv[2] if len(sys.argv) > 2 else ""

for f in sorted(glob.glob(os.path.join(sess_dir, "*.jsonl"))):
    sid = os.path.basename(f)[:8]
    events = []
    with open(f, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            try:
                o = json.loads(line)
            except Exception:
                continue
            if o.get("type") != "assistant":
                continue
            ts = (o.get("timestamp") or "")[:16].replace("T", " ")
            for c in (o.get("message", {}).get("content") or []):
                if isinstance(c, dict) and c.get("type") == "tool_use":
                    n = c.get("name")
                    if n in ("Write", "Edit", "MultiEdit", "NotebookEdit"):
                        fp = (c.get("input") or {}).get("file_path", "?")
                        if proj and proj not in fp:
                            continue
                        events.append((ts, n, fp))
    if events:
        print(f"=== {sid} : {len(events)} 次写入 ===")
        for ts, n, fp in events:
            print(f"   {ts} {n:5} {fp}")
```

**同时要能回答"改的是什么"** —— 只有 `Write`（整写）和 `Edit`（局部改）之分还不够，要打出 `old_string` / `new_string` 的摘要。8GodOS 就是靠这个查出：09-11 对 `boot.asm` 的 9 次 Edit **是纯删注释、指令行逐字未变**。

```python
# 在 scan_writes.py 基础上, 打印指定会话对指定文件的每次改动摘要
import json, glob
f = glob.glob("<会话目录>/<会话ID前8位>*.jsonl")[0]
n = 0
with open(f, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        try:
            o = json.loads(line)
        except Exception:
            continue
        if o.get("type") != "assistant":
            continue
        for c in (o.get("message", {}).get("content") or []):
            if not (isinstance(c, dict) and c.get("type") == "tool_use"
                    and c.get("name") == "Edit"):
                continue
            inp = c.get("input") or {}
            if "<目标文件名>" not in str(inp.get("file_path", "")):
                continue
            n += 1
            o_s = str(inp.get("old_string", ""))
            n_s = str(inp.get("new_string", ""))
            print("--- [%d] old=%d字符 -> new=%d字符" % (n, len(o_s), len(n_s)))
            print("    OLD: " + " ".join(o_s.split())[:150])
            print("    NEW: " + " ".join(n_s.split())[:150])
```

**还要统计模型版本** —— 规则第 1 条要求说明「用了哪些 AI 工具和**大模型**」：

```python
import json, glob, collections
for f in sorted(glob.glob("<会话目录>/*.jsonl")):
    sid = f.split("/")[-1][:8]
    m = collections.Counter()
    with open(f, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            try:
                o = json.loads(line)
            except Exception:
                continue
            if o.get("type") == "assistant" and (o.get("message") or {}).get("model"):
                m[o["message"]["model"]] += 1
    if m:
        print(sid, dict(m))
```

---

## 4. 第二个核心问题：队员是否同意 / 人工做了什么

光知道"AI 写了"不够 —— 规则第 3 条要的是「**哪些由队员人工修改、重构、验证**」。需要把 AI 的写操作和**队员当时的原话**对上。

方法：打印队员消息，定位到写操作之前的那条请求。

```python
import json, glob
f = glob.glob("<会话目录>/<会话ID前8位>*.jsonl")[0]
with open(f, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        try:
            o = json.loads(line)
        except Exception:
            continue
        if o.get("type") != "user":
            continue
        ts = (o.get("timestamp") or "")[:16].replace("T", " ")
        c = o.get("message", {}).get("content")
        if isinstance(c, str):
            s = c
        elif isinstance(c, list):
            s = " ".join(x.get("text", "") for x in c
                         if isinstance(x, dict) and x.get("type") == "text")
        else:
            s = ""
        s = " ".join(s.split())
        if not s or "system-reminder" in s[:120] or s.startswith("<"):
            continue          # 滤掉系统注入的消息, 否则会误当成队员发言
        print("[%s] %s" % (ts, s[:150]))
```

**这一列过滤条件很重要**：会话文件里混着大量系统提醒、工具结果，不过滤就会把系统的声音记成队员的。

8GodOS 靠这个方法查清了：M5 的两次 AI 整写，前面都有队员的明确请求（「这些你直接帮我写吧」「剩下的你直接帮我写吧,今晚M5要完工」）；而 09-11 删注释也是队员要求的（「把注释全删了吧」）—— **最后这条纠正了一个冤案，见第 6 节**。

---

## 5. 记录文档骨架

`docs/AI-使用记录.md` 按规则的五条组织，**一一对应**，让审查者好核：

| 节 | 对应规则 | 内容 |
|---|---|---|
| 一、使用的 AI 工具 | 第 1 条 | 工具 / 模型 / 使用方式 / 起止 + **逐会话模型分布表** |
| 二、逐文件来源标注 | 第 3 条 | 每个源码文件：队员手写 / AI 生成 / 混合，并说明依据 |
| 三、逐次记录 | 第 2 条 | 按时间：队员请求原文 → AI 做了什么 → 队员做了什么 |
| 四、AI 生成内容的错误与发现方式 | 第 4 条 | 错误 / 谁写的 / **如何发现** / 如何修正。**AI 造成的实际损失也要写** |
| 五、设计逻辑与技术细节 | 第 5 条 | 指向设计文档；列出队员应当能现场回答的问题 |
| 六、交流记录索引 | 第 2 条 | 会话表：时间 / ID / 消息数 / 主题 |
| 七、可核验性说明 | — | 本节结论怎么得出的、用什么脚本、还有什么没查清 |
| 八、更正记录 | — | **记录自己出错时的更正史**，见第 6 节 |

**源码侧**：AI 参与过的文件顶部加来源声明注释块（8GodOS 的做法：

```c
/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   队员编写: ...
 *   AI 生成: ...（日期 + 对应里程碑）
 *   队员验证: ...（用什么手段验的）
 */
```

）**未参与的文件不要加** —— 声明是有信息量的，滥加就等于没有。

---

## 6. 三条踩过的坑（这一节最值钱）

### 坑 1：不要凭空认罪

留痕初稿把「AI 在 09-11 删除了队员写在 `boot.asm` 里的注释」登记为 **AI 越界删改**。
回查上下文发现：那是队员 11:19 的明确要求（「把注释全删了吧」）。

**为什么危险**：虚假记录不只包括**隐瞒**，也包括**凭空揽责**。一份主动认罪的记录，会让审查者怀疑它在凑数、在表演诚实。

### 坑 2：不要把来源不明的说法当成硬性要求

初稿写了「赛规要求在 git commit / 开发文档 / 设计文档 / 答辩 PPT 中说明」。
拿到通知原文后逐条核对：**原文根本没提渠道**，只说"明确说明"五项内容。那串渠道来自网络检索摘要。

**为什么危险**：拿着一个不存在的标准去指导项目，等于给自己加戏；万一审查者据此追问"你的答辩 PPT 呢"，你会发现自己准备错了方向。

### 坑 3：正确判断"谁写的"，不要靠直觉和 git

初稿断言「M1~M5 代码全部由队员手写，AI 只做设计讨论」。
扫描 `tool_use` 后发现：AI 在 09-14 整写 `vga.h`/`kernel.c`、09-15 整写 `vga.c`，远早于 M6。

**为什么危险**：这条要是没查出来，就是**实打实的虚假记录** —— 而虚假记录的后果是取消资格。

### 由此得出的规矩

> **记录里的每一条事实性断言，都要能指回它的原始出处。**
> 指不回去的，标「待核对」或者直接删掉 —— 不要用"应该是""大概是"填。

---

## 7. 新项目启动清单

- [ ] 拿到**当赛季**规则原文，确认年份，抄录并标出处（含它引用的其他文档）
- [ ] 建 `docs/AI-使用记录.md` + `docs/AI交流记录/`，`.gitignore` 排除 `原始记录/`
- [ ] **从第一次让 AI 写代码起就开始记** —— 事后追溯要花几倍力气（8GodOS 追溯花了整整一轮）
- [ ] 每次 AI 写代码：commit message 加披露行 + 更新记录第三节 + 检查第二节的逐文件表
- [ ] 每完成一个里程碑：补 `docs/AI交流记录/` 的片段，跑一次第 3 节的扫描脚本复核
- [ ] 每次 AI 出错（包括 AI 造成的损失）：记进第四节，写清**如何发现**
- [ ] 记录本身出错时：记进第八节，**保留错误原文**，不静默改写
- [ ] 交付前：跑一遍第 3、4 节的全部脚本，逐条验证记录与证据一致
