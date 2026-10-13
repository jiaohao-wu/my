"""小豪 · 智能体工作台（对话框升级版）

- 对话框改成「气泡消息流」：文字与图片混排，我发的图、小豪回的图都能直接看到
- 输入侧：添加图片 / Ctrl+V 粘贴剪贴板图片，附件条带缩略图，发送后图片进气泡
- 输出侧：小豪回的图片（data URI、图片 URL、Markdown 图片语法）自动落盘并渲染
- 点任意缩略图 → 大图查看器（左右翻页、适应窗口 / 1:1、打开、另存为、Esc 关闭）
- 请求层（build_payload / stream_agent_reply）与界面层分离，便于单独自测

依赖：requests、Pillow（缩略图与大图查看需要；缺失时自动退化为仅显示文件名）
"""

import base64
import json
import mimetypes
import os
import queue
import re
import tempfile
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import requests

API_URL = "https://rv432pxgmh.coze.site/stream_run"
PROJECT_ID = "7684861411763241000"
DEFAULT_TOKEN = "eyJhbGciOiJSUzI1NiIsImtpZCI6ImE4ZWQ1N2ZkLWI1NjEtNDA1YS1hNTQ3LTVjYzkxYmNlYmQwOSJ9.eyJpc3MiOiJodHRwczovL2FwaS5jb3plLmNuIiwiYXVkIjpbIjM3bEs0UlRIeFBrZlVxdEI5QVdObmZUa0tVMTg4MVRKIl0sImV4cCI6ODIxMDI2Njg3Njc5OSwiaWF0IjoxNzg5MzkyMjQzLCJzdWIiOiJzcGlmZmU6Ly9hcGkuY296ZS5jbi93b3JrbG9hZF9pZGVudGl0eS9pZDo3Njg0OTA1MTI5MjUzNzk3OTIyIiwic3JjIjoiaW5ib3VuZF9hdXRoX2FjY2Vzc190b2tlbl9pZDo3Njg1MzgxMTY3MzE0MzcwNjAzIn0.c3nNcg_QlYSTOvc1novzxbWd9yAojyTNauF96HYuboa0tXKScIMozNxQmbbMoWJUdHowsvIKhkBjMH2-BaNblUfksUY6zBpB4vJxi0DF-3Rc1qJ_wdyBfJfBC9GHK8uVRyBK2voLLRv6qlGWRe90iFnGQMgjnnEIQ_IIKnsC704J4U1iK9hQPg1pRqVzOHp_mLZfsDYmECZa4dbOCWVGhWO2yRqD1__ScjZSXRX_WPIU4kwckqtD5YbjTiWlXx_Pl7L4-h0EIei8bCmZBR_15SYLq2caudBt_LUR1_ZIVoJdnuVsXzPfYrqSXfkGxeoedA_dY1ugXcBgnahoaF13Lg"
MAX_IMAGES = 4                  # 单条消息最多携带几张图片
IMAGE_TYPES = "*.png *.jpg *.jpeg *.gif *.webp *.bmp"
THUMB_MAX = 260                 # 气泡内缩略图最长边
CHIP_THUMB = 40                 # 附件条缩略图边长
GRID_COLS = 2                   # 气泡内图片每行几张
IMAGE_EXT = ("png", "jpg", "jpeg", "gif", "webp", "bmp")

# 一键生成：点一下就把预设提示词直接发给小豪（按钮文案, 提示词）
QUICK_PROMPTS = [
    ("生成敌人卡牌", "直接给我生成一个敌人卡牌"),
    ("生成一个英雄卡牌", "直接生成一个英雄卡牌"),
]

# --------------------------- 主题（深色 · 简约 · 高级） ---------------------------
BG = "#0f1014"          # 窗口底
PANEL = "#16181f"       # 面板（对话区底）
PANEL_HI = "#1d2029"    # 面板高亮（悬停 / 输入框）
BORDER = "#262a34"
BUBBLE_ME = "#1b2433"   # 我的气泡
BUBBLE_AI = "#1a1d25"   # 小豪的气泡
TEXT = "#e8e9ee"
TEXT_DIM = "#9aa0ac"
TEXT_FAINT = "#6b7280"
ACCENT = "#86d98a"      # 与启动器同款强调绿
ACCENT_DK = "#121a16"
BLUE = "#7aa2dc"        # 与启动器按钮图标同色
WARN = "#f0b429"
DANGER = "#e06a6a"
SCROLL_BAR = "#3d4557"
SCROLL_HOVER = "#4a5468"
FONT = "Microsoft YaHei UI"

# 流式文本里可能夹带的图片：Markdown 语法 或 裸图片链接
IMG_PATTERN = re.compile(
    r"!\[[^\]]*\]\(\s*<?("
    r"https?://[^\s)>]+|data:image/[^\s)>;]+;base64,[A-Za-z0-9+/=]+"
    r")>?\s*\)"
    r"|(?<!\()("
    r"https?://[^\s<>\"')\]]+\.(?:png|jpe?g|gif|webp|bmp)(?:\?[^\s<>\"')\]]*)?"
    r")",
    re.IGNORECASE,
)


def default_output_dir():
    """作品默认存到脚本同级的「小豪作品」目录，失败则退回下载目录。"""
    base = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.join(base, "小豪作品")
    try:
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except OSError:
        fallback = os.path.join(os.path.expanduser("~"), "Downloads", "agent_workbench_outputs")
        os.makedirs(fallback, exist_ok=True)
        return fallback


def encode_image(path):
    """读图 → base64 摘要（供请求体内联）"""
    mime_type, _ = mimetypes.guess_type(path)
    if not mime_type or not mime_type.startswith("image/"):
        return None
    with open(path, "rb") as image_file:
        return {
            "name": os.path.basename(path),
            "mime_type": mime_type,
            "data": base64.b64encode(image_file.read()).decode("ascii"),
        }


# =====================================================================
# 请求层（与界面无关，可单独调用做自测）
# =====================================================================
def build_prompt_items(prompt, images):
    """把文字与图片组装成接口要求的 prompt 数组。"""
    items = []
    if prompt:
        items.append({"type": "text", "content": {"text": prompt}})
    for image in images or []:
        items.append({
            "type": "image",
            "content": {"image_url": f"data:{image['mime_type']};base64,{image['data']}"},
        })
    return items


def build_payload(prompt, images, session_id):
    return {
        "content": {"query": {"prompt": build_prompt_items(prompt, images)}},
        "type": "query",
        "session_id": session_id,
        "project_id": PROJECT_ID,
    }


def stream_agent_reply(payload, token, timeout=(15, 300)):
    """发起流式请求，逐条 yield SSE 的 data 负载。"""
    response = requests.post(
        API_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
        },
        json=payload,
        stream=True,
        timeout=timeout,
    )
    response.raise_for_status()
    for line in response.iter_lines(decode_unicode=True):
        if line and line.startswith("data:"):
            yield line[5:].strip()


# =====================================================================
# 图片查看器
# =====================================================================
class ImageViewer(tk.Toplevel):
    """大图查看：左右翻页、适应窗口 / 1:1、打开、另存为、Esc 关闭。"""

    def __init__(self, master, paths, index=0):
        super().__init__(master)
        self.paths = [p for p in paths if p and os.path.exists(p)]
        if not self.paths:
            self.destroy()
            raise ValueError("没有可预览的图片")
        self.index = max(0, min(index, len(self.paths) - 1))
        self.fit_window = True
        self._photo = None

        self.title("图片预览")
        self.configure(bg=BG)
        self.transient(master)
        self.geometry(self._initial_geometry())
        self.minsize(420, 320)

        head = tk.Frame(self, bg=PANEL, height=46)
        head.pack(fill="x")
        head.pack_propagate(False)
        self.title_label = tk.Label(head, text="", bg=PANEL, fg=TEXT,
                                    font=(FONT, 11, "bold"))
        self.title_label.pack(side="left", padx=16)
        self.count_label = tk.Label(head, text="", bg=PANEL, fg=TEXT_FAINT, font=(FONT, 9))
        self.count_label.pack(side="right", padx=16)

        stage = tk.Frame(self, bg=BG)
        stage.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(stage, bg=BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=8, pady=8)
        self.canvas.bind("<Button-1>", self._drag_start)
        self.canvas.bind("<B1-Motion>", self._drag_move)
        self.canvas.bind("<MouseWheel>", self._wheel)

        foot = tk.Frame(self, bg=PANEL, height=52)
        foot.pack(fill="x")
        foot.pack_propagate(False)
        self._flat(foot, "◀ 上一张", self.prev_image).pack(side="left", padx=(14, 6), pady=9)
        self._flat(foot, "下一张 ▶", self.next_image).pack(side="left", padx=(0, 6))
        self.zoom_button = self._flat(foot, "1:1", self.toggle_zoom)
        self.zoom_button.pack(side="left", padx=(0, 6))
        self._flat(foot, "另存为", self.save_as).pack(side="right", padx=(6, 14))
        self._flat(foot, "打开", self.open_current, fg=ACCENT).pack(side="right", padx=(6, 0))

        self.bind("<Escape>", lambda _e: self.destroy())
        self.bind("<Left>", lambda _e: self.prev_image())
        self.bind("<Right>", lambda _e: self.next_image())
        self.bind("<Configure>", self._on_resize)
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self._render()
        self.focus_force()

    # ---------------------- 基础 ----------------------
    def _flat(self, parent, text, command, fg=TEXT_DIM):
        button = tk.Button(
            parent, text=text, command=command, bg=PANEL_HI, fg=fg,
            activebackground=PANEL_HI, activeforeground=ACCENT,
            relief="flat", borderwidth=0, highlightthickness=0,
            cursor="hand2", padx=14, pady=5, font=(FONT, 10),
        )
        button.bind("<Enter>", lambda _e: button.configure(fg=ACCENT))
        button.bind("<Leave>", lambda _e: button.configure(fg=fg))
        return button

    @staticmethod
    def _initial_geometry():
        width = int(ImageViewer._screen_w() * 0.72)
        height = int(ImageViewer._screen_h() * 0.78)
        x = (ImageViewer._screen_w() - width) // 2
        y = max(20, (ImageViewer._screen_h() - height) // 2 - 20)
        return f"{width}x{height}+{x}+{y}"

    @staticmethod
    def _screen_w():
        return tk._default_root.winfo_screenwidth() if tk._default_root else 1440

    @staticmethod
    def _screen_h():
        return tk._default_root.winfo_screenheight() if tk._default_root else 900

    # ---------------------- 渲染 ----------------------
    def _render(self):
        path = self.paths[self.index]
        self.title_label.configure(text=os.path.basename(path))
        self.count_label.configure(text=f"{self.index + 1} / {len(self.paths)}")
        self.canvas.delete("all")
        try:
            from PIL import Image, ImageTk
            image = Image.open(path)
            if self.fit_window:
                image.thumbnail(self._stage_size(), Image.LANCZOS)
            self._photo = ImageTk.PhotoImage(image)
        except Exception:                                   # noqa: BLE001 - 退化处理
            self.canvas.create_text(
                20, 20, anchor="nw", fill=TEXT_FAINT, font=(FONT, 10),
                text=f"无法预览该图片：\n{path}")
            return
        self.canvas.create_image(0, 0, anchor="nw", image=self._photo)
        self.canvas.configure(scrollregion=(0, 0, self._photo.width(), self._photo.height()))

    def _stage_size(self):
        self.update_idletasks()
        return (max(200, self.canvas.winfo_width() - 4), max(200, self.canvas.winfo_height() - 4))

    def _on_resize(self, event):
        if event.widget is not self:
            return
        if self.fit_window:
            self._render()

    # ---------------------- 交互 ----------------------
    def _wheel(self, event):
        self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        return "break"

    def _drag_start(self, event):
        self.canvas.scan_mark(event.x, event.y)

    def _drag_move(self, event):
        self.canvas.scan_dragto(event.x, event.y, gain=1)

    def toggle_zoom(self):
        self.fit_window = not self.fit_window
        self.zoom_button.configure(text="适应窗口" if not self.fit_window else "1:1")
        self._render()

    def prev_image(self):
        if len(self.paths) > 1:
            self.index = (self.index - 1) % len(self.paths)
            self._render()

    def next_image(self):
        if len(self.paths) > 1:
            self.index = (self.index + 1) % len(self.paths)
            self._render()

    def open_current(self):
        try:
            os.startfile(self.paths[self.index])
        except OSError as error:
            messagebox.showerror("打开失败", str(error), parent=self)

    def save_as(self):
        source = self.paths[self.index]
        extension = os.path.splitext(source)[1] or ".png"
        target = filedialog.asksaveasfilename(
            parent=self, title="另存为",
            initialfile=os.path.basename(source),
            defaultextension=extension,
            filetypes=[("图片文件", IMAGE_TYPES), ("所有文件", "*.*")],
        )
        if not target:
            return
        try:
            with open(source, "rb") as src, open(target, "wb") as dst:
                dst.write(src.read())
        except OSError as error:
            messagebox.showerror("保存失败", str(error), parent=self)


# =====================================================================
# 界面
# =====================================================================
class AgentWindow:
    def __init__(self, root):
        self.root = root
        self.root.title("小豪 · 智能体工作台")
        self.root.geometry("980x720")
        self.root.minsize(780, 560)
        self.root.configure(bg=BG)

        self.events = queue.Queue()
        self.session_id = self._new_session_id()
        self.request_in_progress = False
        self.attachments = []          # [{name, mime_type, data, path}]
        self.handled_media = set()     # 本轮已处理过的作品地址，避免重复保存
        self.last_output_path = None
        self.output_dir = default_output_dir()
        self._photos = []              # 持有 PhotoImage 引用，防止被回收
        self._chip_photos = []
        self._wrap_labels = []         # [(label, min_width)] —— 窗口缩放时统一调整换行宽度
        self._bubble_body = None       # 当前气泡的内容容器
        self._bubble_paths = []        # 当前气泡内的图片路径
        self._bubble_role = None       # "me" / "ai"
        self._text_label = None        # 当前气泡的文字标签
        self._stream_buf = ""          # 流式文本缓冲（按行处理 Markdown 图片）
        self._auto_scroll = True

        self._apply_dark_titlebar()
        self._setup_scrollbar_style()
        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(80, self._process_events)

    # ---------------------------- 基础设施 ----------------------------
    @staticmethod
    def _new_session_id():
        import uuid
        return str(uuid.uuid4())

    def _apply_dark_titlebar(self):
        """Windows 10/11 深色标题栏（拿不到就静默跳过）"""
        try:
            import ctypes
            value = ctypes.c_int(1)
            self.root.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            for attribute in (20, 19):   # DWMWA_USE_IMMERSIVE_DARK_MODE / 旧值
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attribute, ctypes.byref(value), ctypes.sizeof(value))
        except Exception:
            pass

    @staticmethod
    def _setup_scrollbar_style():
        """深色细滚动条（去掉箭头，只留滑块）"""
        try:
            style = ttk.Style()
            style.theme_use("clam")
            style.layout("Dark.Vertical.TScrollbar", [
                ("Vertical.Scrollbar.trough", {
                    "children": [("Vertical.Scrollbar.thumb", {"expand": "1", "sticky": "ns"})],
                    "sticky": "ns"}),
            ])
            style.configure("Dark.Vertical.TScrollbar", troughcolor=PANEL,
                            background=SCROLL_BAR, borderwidth=0, arrowsize=0,
                            relief="flat", width=9)
            style.map("Dark.Vertical.TScrollbar",
                      background=[("pressed", SCROLL_HOVER), ("active", SCROLL_HOVER)])
        except Exception:
            pass

    def _flat_button(self, parent, text, command, fg=TEXT, bg=PANEL_HI,
                     hover=ACCENT, width=None, font_size=10, bold=False):
        """统一风格的无边框按钮（Tk 没有圆角，用留白与配色做出克制感）"""
        button = tk.Button(
            parent, text=text, command=command,
            bg=bg, fg=fg, activebackground=bg, activeforeground=hover,
            relief="flat", borderwidth=0, highlightthickness=0,
            cursor="hand2", padx=14, pady=6,
            font=(FONT, font_size, "bold" if bold else "normal"),
        )
        if width:
            button.configure(width=width)
        button.bind("<Enter>", lambda _e: button.configure(fg=hover))
        button.bind("<Leave>", lambda _e: button.configure(fg=fg))
        return button

    # ------------------------------ 布局 ------------------------------
    def _build_ui(self):
        # ---- 顶栏 ----
        header = tk.Frame(self.root, bg=PANEL, height=62)
        header.pack(fill="x")
        header.pack_propagate(False)
        tk.Frame(header, bg=ACCENT, width=3).pack(side="left", fill="y")
        title_box = tk.Frame(header, bg=PANEL)
        title_box.pack(side="left", padx=(16, 0))
        tk.Label(title_box, text="小豪", bg=PANEL, fg=TEXT,
                 font=(FONT, 17, "bold")).pack(anchor="w", pady=(10, 0))
        tk.Label(title_box, text="智能体工作台 · 文字与图片对话", bg=PANEL, fg=TEXT_FAINT,
                 font=(FONT, 9)).pack(anchor="w")

        self.status_label = tk.Label(header, text="● 就绪", bg=PANEL, fg=ACCENT,
                                     font=(FONT, 10))
        self.status_label.pack(side="right", padx=22)

        # ---- 工具条 ----
        toolbar = tk.Frame(self.root, bg=BG)
        toolbar.pack(fill="x", padx=18, pady=(12, 6))
        self._flat_button(toolbar, "＋ 添加图片", self.choose_images,
                          fg=ACCENT, bg=PANEL_HI).pack(side="left")
        self._flat_button(toolbar, "粘贴图片", self.paste_image,
                          fg=TEXT_DIM, bg=PANEL_HI).pack(side="left", padx=(8, 0))
        self.image_hint = tk.Label(toolbar, text=f"最多 {MAX_IMAGES} 张 · Ctrl+V 可直接粘贴",
                                   bg=BG, fg=TEXT_FAINT, font=(FONT, 9))
        self.image_hint.pack(side="left", padx=(10, 0))
        self.open_output_button = self._flat_button(
            toolbar, "打开作品", self.open_last_output, fg=TEXT_DIM, bg=PANEL_HI)
        self.open_output_button.configure(state="disabled", fg=TEXT_FAINT)
        self.open_output_button.pack(side="right")
        self._flat_button(toolbar, "作品目录", self.open_output_dir,
                          fg=TEXT_DIM, bg=PANEL_HI).pack(side="right", padx=(0, 8))
        self._flat_button(toolbar, "新建会话", self.new_session,
                          fg=TEXT_DIM, bg=PANEL_HI).pack(side="right", padx=(0, 8))
        self._flat_button(toolbar, "返回", self.root.destroy,
                          fg=DANGER, bg=PANEL_HI).pack(side="right", padx=(0, 8))

        # ---- 一键生成（三个快捷指令）----
        quick = tk.Frame(self.root, bg=BG)
        quick.pack(fill="x", padx=18, pady=(0, 4))
        tk.Label(quick, text="一键生成", bg=BG, fg=TEXT_FAINT,
                 font=(FONT, 9)).pack(side="left", padx=(0, 10))
        self.quick_buttons = []
        for text, prompt in QUICK_PROMPTS:
            button = self._flat_button(quick, text,
                                       lambda p=prompt: self.send_prompt(p),
                                       fg=ACCENT, bg=PANEL_HI)
            button.pack(side="left", padx=(0, 8))
            self.quick_buttons.append(button)

        # ---- 附件条（选中的图片缩略图）----
        self.attach_strip = tk.Frame(self.root, bg=BG)

        # ---- 对话区（气泡消息流）----
        chat_box = tk.Frame(self.root, bg=BORDER, bd=0)
        chat_box.pack(fill="both", expand=True, padx=18, pady=(4, 10))
        self.canvas = tk.Canvas(chat_box, bg=PANEL, highlightthickness=0, bd=0)
        self.canvas.pack(side="left", fill="both", expand=True, padx=1, pady=1)
        self.scrollbar = ttk.Scrollbar(chat_box, orient="vertical",
                                       style="Dark.Vertical.TScrollbar",
                                       command=self.canvas.yview)
        self.scrollbar.pack(side="right", fill="y", padx=(0, 1), pady=1)
        self.canvas.configure(yscrollcommand=self._on_scroll)

        self.messages = tk.Frame(self.canvas, bg=PANEL)
        self._messages_window = self.canvas.create_window((0, 0), window=self.messages,
                                                          anchor="nw")
        self.messages.bind("<Configure>", self._on_messages_configure)
        self.canvas.bind("<Configure>", self._on_canvas_configure)
        self.root.bind_all("<MouseWheel>", self._on_mousewheel)

        self._append_system("会话已就绪 —— 直接输入文字，或先「添加图片 / Ctrl+V 粘贴」再发送。")

        # ---- 输入区 ----
        composer = tk.Frame(self.root, bg=BG)
        composer.pack(fill="x", padx=18, pady=(0, 6))
        input_box = tk.Frame(composer, bg=PANEL_HI, highlightthickness=1,
                             highlightbackground=BORDER, highlightcolor=ACCENT)
        input_box.pack(side="left", fill="both", expand=True)
        self.input_box = tk.Text(
            input_box, height=4, wrap=tk.WORD, bg=PANEL_HI, fg=TEXT,
            insertbackground=ACCENT, relief="flat", borderwidth=0,
            font=(FONT, 11), padx=12, pady=10,
        )
        self.input_box.pack(fill="both", expand=True)
        self.input_box.bind("<Control-Return>", self._send_from_event)
        self.input_box.bind("<Return>", self._send_from_event)
        self.input_box.bind("<Control-v>", self.paste_image)
        self.input_box.bind("<Control-V>", self.paste_image)

        side = tk.Frame(composer, bg=BG)
        side.pack(side="right", fill="y", padx=(10, 0))
        self.send_button = tk.Button(
            side, text="发送", command=self.send_message,
            bg=ACCENT, fg=ACCENT_DK, activebackground="#9ce8a2", activeforeground=ACCENT_DK,
            relief="flat", borderwidth=0, highlightthickness=0,
            cursor="hand2", width=10, height=3, font=(FONT, 11, "bold"),
        )
        self.send_button.pack(fill="both", expand=True)

        footer = tk.Label(self.root, bg=BG, fg=TEXT_FAINT, font=(FONT, 8),
                          text=f"Enter 发送 · Shift+Enter 换行 · Ctrl+V 粘贴图片 · "
                               f"点缩略图看大图 · 作品保存到 {self.output_dir}")
        footer.pack(fill="x", padx=20, pady=(0, 10))

    # ---------------------------- 滚动 ----------------------------
    def _on_messages_configure(self, _event):
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        if self._auto_scroll:
            self.canvas.yview_moveto(1.0)

    def _on_canvas_configure(self, event):
        self.canvas.itemconfigure(self._messages_window, width=event.width)
        self._update_wrap_length()
        if self._auto_scroll:
            self.canvas.yview_moveto(1.0)

    def _on_scroll(self, first, _last):
        self.scrollbar.set(first, _last)
        self._auto_scroll = float(first) <= 0.0 or float(_last) >= 0.999

    def _on_mousewheel(self, event):
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if widget is not None and self._is_descendant(widget, self.input_box):
            self.input_box.yview_scroll(int(-1 * (event.delta / 120)), "units")
            return "break"
        if widget is None or not self._is_descendant(widget, self.canvas):
            return None
        self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        return "break"

    @staticmethod
    def _is_descendant(widget, ancestor):
        while widget is not None:
            if widget == ancestor:
                return True
            widget = widget.master
        return False

    def _scroll_to_bottom(self):
        self.canvas.update_idletasks()
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        if self._auto_scroll:
            self.canvas.yview_moveto(1.0)

    def _update_wrap_length(self):
        """窗口缩放时统一调整气泡文字的换行宽度（气泡不超过对话区宽度的 76%）"""
        available = self.canvas.winfo_width() - 70
        width = max(200, min(int(available * 0.76), 720))
        for label in self._wrap_labels:
            try:
                label.configure(wraplength=width)
            except tk.TclError:
                pass

    # ---------------------------- 气泡渲染 ----------------------------
    def _append_system(self, text):
        row = tk.Frame(self.messages, bg=PANEL)
        row.pack(fill="x", padx=20, pady=(10, 2))
        label = tk.Label(row, text=f"—— {text} ——", bg=PANEL, fg=TEXT_FAINT,
                         font=(FONT, 9), justify="center")
        label.pack(fill="x")
        self._wrap_labels.append(label)
        self._update_wrap_length()
        self._scroll_to_bottom()

    def _finish_bubble(self):
        """结束当前气泡（切换角色或回复结束时调用）"""
        if self._stream_buf:
            self._render_line(self._stream_buf)
            self._stream_buf = ""
        self._bubble_body = None
        self._bubble_role = None
        self._text_label = None
        self._bubble_paths = []

    def _new_bubble(self, role):
        """创建一条消息气泡，返回内容容器 Frame。"""
        self._finish_bubble()
        is_me = role == "me"
        row = tk.Frame(self.messages, bg=PANEL)
        row.pack(fill="x", padx=16, pady=(12, 2))
        inner = tk.Frame(row, bg=PANEL)
        inner.pack(anchor="e" if is_me else "w", fill="x")
        tk.Label(inner, text="我" if is_me else "小豪", bg=PANEL,
                 fg=BLUE if is_me else ACCENT,
                 font=(FONT, 9, "bold")).pack(anchor="e" if is_me else "w")

        bubble_bg = BUBBLE_ME if is_me else BUBBLE_AI
        bubble = tk.Frame(inner, bg=bubble_bg, highlightthickness=1,
                          highlightbackground=BORDER)
        bubble.pack(anchor="e" if is_me else "w", pady=(4, 0))
        body = tk.Frame(bubble, bg=bubble_bg)
        body.pack(fill="both", expand=True, padx=13, pady=10)

        self._bubble_body = body
        self._bubble_role = role
        self._bubble_paths = []
        self._text_label = None
        self._scroll_to_bottom()
        return body

    def _ensure_agent_bubble(self):
        if self._bubble_role == "ai" and self._bubble_body is not None \
                and self._bubble_body.winfo_exists():
            return self._bubble_body
        return self._new_bubble("ai")

    def _append_text(self, text):
        """在当前气泡里追加一段文字（没有文字标签就先建一个）。"""
        if not text:
            return
        if self._text_label is None or not self._text_label.winfo_exists():
            is_me = self._bubble_role == "me"
            self._text_label = tk.Label(
                self._bubble_body, text="", bg=self._bubble_body.cget("bg"),
                fg="#cdd6e4" if is_me else TEXT, font=(FONT, 11),
                justify="right" if is_me else "left",
                anchor="e" if is_me else "w")
            self._text_label.pack(fill="x")
            self._wrap_labels.append(self._text_label)
            self._update_wrap_length()
        self._text_label.configure(text=self._text_label.cget("text") + text)

    def _append_user(self, text, image_paths):
        body = self._new_bubble("me")
        self._append_text(text or "（图片）")
        if image_paths:
            self._insert_images(body, image_paths)
        self._scroll_to_bottom()

    def _insert_images(self, body, paths):
        """在指定气泡里以网格插入缩略图，点击可查看大图。"""
        if not paths or body is None or not body.winfo_exists():
            return
        bg = body.cget("bg")
        grid = tk.Frame(body, bg=bg)
        grid.pack(fill="x", pady=(9, 2))
        for index, path in enumerate(paths):
            row, column = divmod(index, GRID_COLS)
            cell = tk.Label(grid, bg=PANEL_HI, cursor="hand2",
                            highlightthickness=1, highlightbackground=BORDER)
            cell.grid(row=row, column=column, padx=(0, 6 if column == 0 else 0),
                      pady=(0, 6), sticky="w")
            photo = self._load_photo(path, THUMB_MAX)
            if photo is None:
                cell.configure(text="[无法预览]", fg=TEXT_FAINT, font=(FONT, 9),
                               padx=12, pady=12)
            else:
                self._photos.append(photo)      # 持有引用，避免被 GC 回收
                cell.configure(image=photo)
            cell.bind("<Button-1>",
                      lambda _e, idx=index: self._open_viewer(paths, idx))
            self._bubble_paths.append(path)
        self._scroll_to_bottom()

    def _open_viewer(self, paths, index):
        valid = [p for p in paths if p and os.path.exists(p)]
        if not valid:
            messagebox.showinfo("图片不存在", "这张图片已不在原位置，无法预览。")
            return
        try:
            ImageViewer(self.root, valid, min(index, len(valid) - 1))
        except Exception as error:                          # noqa: BLE001
            messagebox.showerror("预览失败", str(error))

    def _load_photo(self, path, max_side):
        """能预览就预览，不能就返回 None（Pillow 缺失时退化为 PNG 预览）"""
        try:
            from PIL import Image, ImageTk
            image = Image.open(path)
            image.draft("RGB", (max_side, max_side))
            image.thumbnail((max_side, max_side), Image.LANCZOS)
            return ImageTk.PhotoImage(image)
        except Exception:
            pass
        try:
            photo = tk.PhotoImage(file=path)
            factor = max(1, (max(photo.width(), photo.height()) + max_side - 1) // max_side)
            if factor > 1:
                photo = photo.subsample(factor, factor)
            return photo
        except Exception:
            return None

    # ---------------------------- 附件管理 ----------------------------
    def _refresh_attachment_strip(self):
        for child in self.attach_strip.winfo_children():
            child.destroy()
        self.attach_strip.pack_forget()
        self._chip_photos = []          # 附件条重建，旧的缩略图引用一起释放
        self.image_hint.configure(
            text=f"已选 {len(self.attachments)}/{MAX_IMAGES} 张" if self.attachments
            else f"最多 {MAX_IMAGES} 张 · Ctrl+V 可直接粘贴")

        if not self.attachments:
            return
        self.attach_strip.pack(fill="x", padx=18, pady=(0, 4), before=self.canvas.master)
        for index, item in enumerate(self.attachments):
            chip = tk.Frame(self.attach_strip, bg=PANEL_HI, highlightthickness=1,
                            highlightbackground=BORDER)
            chip.pack(side="left", padx=(0, 8))
            photo = self._load_photo(item["path"], CHIP_THUMB)
            if photo is not None:
                self._chip_photos.append(photo)
                thumb = tk.Label(chip, image=photo, bg=PANEL_HI, cursor="hand2")
                thumb.pack(side="left", padx=(6, 4), pady=5)
                thumb.bind("<Button-1>",
                           lambda _e, i=index: self._open_viewer(
                               [item["path"] for item in self.attachments], i))
            name = item["name"]
            if len(name) > 16:
                name = name[:13] + "..."
            tk.Label(chip, text=name, bg=PANEL_HI, fg=TEXT_DIM,
                     font=(FONT, 9)).pack(side="left", padx=(0, 6))
            tk.Label(chip, text="✕", bg=PANEL_HI, fg=TEXT_FAINT, cursor="hand2",
                     font=(FONT, 9, "bold")).pack(side="left", padx=(0, 8))
            chip.winfo_children()[-1].bind(
                "<Button-1>", lambda _e, i=index: self.remove_attachment(i))

    def choose_images(self):
        if len(self.attachments) >= MAX_IMAGES:
            messagebox.showinfo("已达上限", f"单条消息最多 {MAX_IMAGES} 张图片。")
            return
        paths = filedialog.askopenfilenames(
            title="选择要发给小豪的图片",
            filetypes=[("图片文件", IMAGE_TYPES), ("所有文件", "*.*")],
        )
        if not paths:
            return
        self._add_attachment_paths(paths)

    def paste_image(self, event=None):
        """Ctrl+V：剪贴板里有图片就直接作为附件，否则走默认文本粘贴。"""
        try:
            from PIL import ImageGrab
            data = ImageGrab.grabclipboard()
        except Exception as error:                          # noqa: BLE001
            self._append_system(f"读取剪贴板失败：{error}")
            return "break"
        paths = []
        if isinstance(data, list):
            paths = [p for p in data if os.path.isfile(p)]
        elif hasattr(data, "save"):        # PIL Image（截图 / 复制的图像）
            clip_dir = os.path.join(tempfile.gettempdir(), "xiaohao_clipboard")
            os.makedirs(clip_dir, exist_ok=True)
            target = os.path.join(clip_dir, f"paste_{self._new_session_id()[:8]}.png")
            try:
                data.save(target, "PNG")
                paths = [target]
            except Exception as error:                      # noqa: BLE001
                self._append_system(f"剪贴板图片保存失败：{error}")
                return "break"
        if not paths:
            return None                    # 不是图片 → 交还给 Tk 做文本粘贴
        self._add_attachment_paths(paths)
        return "break"

    def _add_attachment_paths(self, paths):
        skipped = 0
        for path in paths:
            if len(self.attachments) >= MAX_IMAGES:
                skipped += 1
                continue
            try:
                encoded = encode_image(path)
            except OSError:
                encoded = None
            if not encoded:
                skipped += 1
                continue
            encoded["path"] = path
            self.attachments.append(encoded)
        self._refresh_attachment_strip()
        if skipped:
            self.image_hint.configure(
                text=f"已选 {len(self.attachments)}/{MAX_IMAGES} 张（{skipped} 张超出上限或不可读）")

    def remove_attachment(self, index):
        if 0 <= index < len(self.attachments):
            self.attachments.pop(index)
            self._refresh_attachment_strip()

    def _clear_attachments(self):
        self.attachments = []
        self._refresh_attachment_strip()

    # ---------------------------- 收发消息 ----------------------------
    def _send_from_event(self, event):
        # Enter 发送、Shift+Enter 换行
        if event.state & 0x0001:
            return None
        self.send_message()
        return "break"

    def send_message(self):
        if self.request_in_progress:
            return
        prompt = self.input_box.get("1.0", tk.END).strip()
        if not prompt and not self.attachments:
            return

        images = list(self.attachments)
        image_paths = [item["path"] for item in images]
        self.input_box.delete("1.0", tk.END)
        self._append_user(prompt, image_paths)     # 气泡里直接显示我发出的图片
        self._clear_attachments()
        self.handled_media.clear()
        self._set_busy(True)

        payload = build_payload(prompt, images, self.session_id)
        threading.Thread(target=self._request_agent, args=(payload,), daemon=True).start()

    def send_prompt(self, prompt):
        """快捷指令：把预设提示词填进输入框并立刻发送（附件保留，可作参考图）。"""
        if self.request_in_progress:
            return
        self.input_box.delete("1.0", tk.END)
        self.input_box.insert("1.0", prompt)
        self.send_message()

    def _request_agent(self, payload):
        token = os.getenv("APL_TOKEN", DEFAULT_TOKEN).strip()
        if not token:
            self.events.put(("error", "未配置 APL_TOKEN：请先设置环境变量再发消息。"))
            return
        try:
            for data_text in stream_agent_reply(payload, token):
                self.events.put(("data", data_text))
            self.events.put(("done", ""))
        except requests.RequestException as error:
            self.events.put(("error", f"网络请求失败：{error}"))
        except Exception as error:                      # noqa: BLE001 - 统一回报给界面
            self.events.put(("error", f"处理响应失败：{error}"))

    def _process_events(self):
        try:
            while True:
                event_type, value = self.events.get_nowait()
                if event_type == "data":
                    self._handle_data(value)
                elif event_type == "image":
                    self._show_output_image(value)
                elif event_type == "system":
                    self._append_system(value)
                elif event_type == "done":
                    self._finish_bubble()
                    self._set_busy(False)
                elif event_type == "error":
                    self._finish_bubble()
                    self._set_busy(False)
                    self._append_system(f"出错了：{value}")
                    messagebox.showerror("调用失败", value)
        except queue.Empty:
            pass
        self.root.after(80, self._process_events)

    def _handle_data(self, data_text):
        try:
            data = json.loads(data_text)
        except json.JSONDecodeError:
            data = data_text
        self._handle_media(data, data_text)
        text = self._find_text(data)
        if text:
            self._append_agent_text(text)

    # ---------------------------- 小豪的文字（流式） ----------------------------
    def _append_agent_text(self, chunk):
        """按行处理：文字进气泡，行内夹带的图片语法/图片链接渲染成图。"""
        self._ensure_agent_bubble()
        self._stream_buf += chunk
        while "\n" in self._stream_buf:
            line, self._stream_buf = self._stream_buf.split("\n", 1)
            self._render_line(line + "\n")

    def _render_line(self, line):
        position = 0
        for match in IMG_PATTERN.finditer(line):
            self._append_text(line[position:match.start()])
            self._emit_media(match.group(1) or match.group(2))
            position = match.end()
        self._append_text(line[position:])

    # ---------------------------- 作品（图片输出） ----------------------------
    def _handle_media(self, value, raw_data):
        media = self._find_media(value)
        if not media and isinstance(raw_data, str) and raw_data.startswith("data:image/"):
            media = raw_data
        if media:
            self._emit_media(media)

    def _emit_media(self, media):
        """收到一个图片地址：data URI 直接落盘，URL 后台下载后回插到原气泡。"""
        if not media or media in self.handled_media:
            return
        self.handled_media.add(media)
        if media.startswith("data:image/"):
            try:
                path = self._save_media(media)
            except (OSError, ValueError) as error:
                self._append_system(f"收到作品，但保存失败：{error}")
                return
            self._register_output(path)
            self._insert_images(self._ensure_agent_bubble(), [path])
        elif media.startswith(("http://", "https://")):
            target = self._ensure_agent_bubble()
            threading.Thread(target=self._fetch_image,
                             args=(media, target), daemon=True).start()

    def _fetch_image(self, url, target):
        try:
            path = self._save_media(url)
        except (OSError, ValueError, requests.RequestException) as error:
            self.events.put(("system", f"图片下载失败：{error}"))
            return
        self.events.put(("image", (path, target)))

    def _show_output_image(self, value):
        path, target = value
        self._register_output(path)
        body = target if (target is not None and target.winfo_exists()) \
            else self._ensure_agent_bubble()
        self._insert_images(body, [path])

    def _register_output(self, path):
        self.last_output_path = path
        self.open_output_button.configure(state="normal", fg=ACCENT)
        self._append_system(f"作品已保存：{path}")

    @staticmethod
    def _find_media(value):
        if isinstance(value, str):
            if value.startswith("data:image/") or value.startswith(("http://", "https://")):
                return value
            return ""
        if isinstance(value, dict):
            for key in ("image_url", "url", "download_url", "file_url"):
                if key in value:
                    result = AgentWindow._find_media(value[key])
                    if result:
                        return result
            for item in value.values():
                result = AgentWindow._find_media(item)
                if result:
                    return result
        if isinstance(value, list):
            for item in value:
                result = AgentWindow._find_media(item)
                if result:
                    return result
        return ""

    def _save_media(self, media):
        output_path = os.path.join(self.output_dir, f"work_{self._new_session_id()[:12]}")
        if media.startswith("data:image/"):
            header, encoded = media.split(",", 1)
            extension = header.split("/", 1)[1].split(";", 1)[0].replace("jpeg", "jpg")
            output_path += f".{extension}"
            with open(output_path, "wb") as output_file:
                output_file.write(base64.b64decode(encoded))
            return output_path
        if media.startswith(("http://", "https://")):
            response = requests.get(media, timeout=(15, 120))
            response.raise_for_status()
            content_type = response.headers.get("Content-Type", "image/png").split(";", 1)[0]
            extension = mimetypes.guess_extension(content_type) or ".png"
            output_path += extension.replace(".jpe", ".jpg")
            with open(output_path, "wb") as output_file:
                output_file.write(response.content)
            return output_path
        raise ValueError("作品返回了无法识别的图片格式")

    @staticmethod
    def _find_text(value):
        if isinstance(value, str):
            return value
        if isinstance(value, dict):
            for key in ("text", "content", "answer", "output", "message"):
                if key in value:
                    result = AgentWindow._find_text(value[key])
                    if result:
                        return result
            return ""
        if isinstance(value, list):
            return "".join(AgentWindow._find_text(item) for item in value)
        return ""

    # ---------------------------- 作品入口 ----------------------------
    def open_last_output(self):
        if self.last_output_path and os.path.exists(self.last_output_path):
            try:
                os.startfile(self.last_output_path)
            except OSError as error:
                messagebox.showerror("打开失败", str(error))

    def open_output_dir(self):
        try:
            os.startfile(self.output_dir)
        except OSError as error:
            messagebox.showerror("打开失败", str(error))

    # ---------------------------- 状态 ----------------------------
    def _set_busy(self, busy):
        self.request_in_progress = busy
        self.send_button.configure(state="disabled" if busy else "normal",
                                   bg=WARN if busy else ACCENT,
                                   text="思考中" if busy else "发送")
        self.status_label.configure(text="● 工作中" if busy else "● 就绪",
                                    fg=WARN if busy else ACCENT)
        for button in self.quick_buttons:      # 思考中也禁掉一键生成，避免重复发送
            button.configure(state="disabled" if busy else "normal",
                             fg=TEXT_FAINT if busy else ACCENT)

    def new_session(self):
        if self.request_in_progress:
            return
        self.session_id = self._new_session_id()
        self.handled_media.clear()
        self.last_output_path = None
        self.open_output_button.configure(state="disabled", fg=TEXT_FAINT)
        self._finish_bubble()
        for child in list(self.messages.winfo_children()):
            child.destroy()
        self._photos.clear()
        self._wrap_labels.clear()
        self._auto_scroll = True
        self._append_system("已新建会话，上下文已重置。")

    def _on_close(self):
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    AgentWindow(root)
    root.mainloop()
