# UE-MCP 工具扩展计划 (Layers 1+2+3)

## 背景与根因
- 现有 61 个工具走纯 RemoteControl HTTP，强于"读场景/改属性/调已暴露 UFunction"，弱于"从零创作资产内部结构"（UMG/蓝图图表）。
- 登录界面卡 27 分钟的根因：`WidgetTree` 在该 5.3 构建无 Python 绑定 + `RootWidget` 是 C++ `protected`。但过程问题更大——逐条 `EvaluateStatement` 往返 ~150 次、同一结论反复确认 6 次、无迭代上限。
- 关键发现：`project_setup.cpp` 已为 UE5 写入 `bIgnoreProtectedCheck=True`，即 RC 写 protected 属性的闸门已开（需重启生效）。这给 UMG 创作留了一条 RC 路径。

## 总体策略
三层推进，全部沿用现有 `register_*()` 分组 + `Tool{name,desc,schema,caps,handler}` 模式：
- **Layer 1**：纯 RC/Python 可做的场景/模型/光照/材质创建/导入工具（低风险，先做）。
- **Layer 2**：服务端 Python "配方层"——成套创作逻辑写成完整脚本，经 `ExecutePythonCommandEx` 一次执行、一次返回结构化结果。干掉"逐条往返+逐条 binding 探测"。
- **Layer 3**：可选的极小引擎内 helper 插件，解决 Python 都搞不定的硬骨头（蓝图节点连线、protected 成员）。本计划只做设计，落地需单独 greenlight。

---

## 核心基础设施（先建，Layer 2 依赖它）

### 1. `run_python_recipe()` 助手（tool_registry.cpp 匿名命名空间）
统一的"参数安全、单次执行、结构化返回"Python 配方执行器：
- 入参 JSON → base64 → 注入脚本头 `_ARGS = json.loads(base64.b64decode("...").decode())`，**杜绝字符串注入**。
- 拼接配方脚本体（脚本体负责构造 `result` dict 并 `print("<<<UEMCP>>>"+json.dumps(result))`）。
- 调 `ExecutePythonCommandEx{ExecutionMode:"ExecuteStatement"}`，扫描返回的 `LogOutput[].Output` 找 sentinel，解析回 JSON。
- 失败时返回结构化 error（含 LogOutput 摘要），不让上层盲目重试。
- 配方脚本以 C++ 原始字符串常量集中存放（仿 `project_setup.cpp` 内嵌 ini 模板的做法）。

### 2. 迭代/容错护栏
- 配方层工具内部已是"单次执行"，天然避免往返爆炸。
- 对 Python 不可用的构建：经现有 `Capability::PythonScripting` 闸门自动降级为 `unsupported`，附带"建议启用 PythonScriptPlugin 或使用 Layer 3 插件"提示。

### 3. 新增 Capability（capability_registry.hpp/.cpp）
- 复用现有 `PythonScripting` 闸门覆盖大部分 Layer 2。
- 新增 `Capability::AssetTools`（probe `AssetToolsHelpers` 可达性，用于创建类配方）。仅在 probe 成功时开启。

---

## Layer 1 — 纯 RC/Python 工具（新增 register 分组）

### `register_scene_tools()`（场景/Actor 增强）
- `ue_batch_spawn_actors` — 走 `/remote/batch` 一次 spawn 多个 actor（建城市/迷宫刚需）。Cap: Batch。
- `ue_duplicate_actor` — `EditorActorSubsystem.DuplicateActor`（modern_then_legacy）。
- `ue_attach_actor` / `ue_detach_actor` — `K2_AttachToActor` / `K2_DetachFromActor`。
- `ue_add_component_to_actor` — `AddComponentByClass`（5.x）。
- `ue_set_actor_folder` — World Outliner 文件夹归类（set `FolderPath` 属性）。
- `ue_get_all_actor_transforms` — 批量拉全场景变换（batch 组合），省往返。

### `register_mesh_light_tools()`（模型/光照）
- `ue_set_static_mesh` — 换 StaticMeshComponent 的 `StaticMesh` 资产引用。
- `ue_set_actor_material` — 给 actor 材质槽直接赋材质资产（`SetMaterial`），比现有 dynamic-param 更基础。
- `ue_get_mesh_sockets` / `ue_get_mesh_bounds`。
- `ue_set_light_intensity` / `ue_set_light_color` — 改 LightComponent 属性。
- `ue_set_directional_light_rotation` — 调太阳角度（找 DirectionalLight + set rotation）。

### `register_creation_tools()`（材质/资产创建，Python 配方）
- `ue_create_material` — `MaterialFactoryNew` 建材质资产。
- `ue_create_material_instance` — 建 MIC（配合现有 `ue_set_material_instance_param`）。
- `ue_import_asset` — FBX/贴图导入（`AssetImportTask`）。
- `ue_create_folder` / `ue_fixup_redirectors`。

### 数据表
- `ue_data_table_get_rows` / `ue_data_table_set_row`（Python 配方）。

### 调试/PIE 补全
- `ue_start_pie`（Cap: PieControl，5.5+ `EditorRequestBeginPlay`）。
- `ue_set_cvar`（写 CVar，补全只读的 `ue_get_console_variable`）。
- `ue_get_log` — 读最近 Output Log（Python 配方）。

---

## Layer 2 — 创作配方工具（`register_authoring_tools()`，全部经 run_python_recipe）

### 蓝图
- `ue_create_blueprint` — `BlueprintFactory` 建蓝图类（指定 parent class、路径）。
- `ue_add_blueprint_variable` — `BlueprintEditorLibrary.add_member_variable`。
- `ue_add_blueprint_component` — 加组件到蓝图。
- `ue_compile_blueprint` — `BlueprintEditorLibrary.compile_blueprint`。

### UMG（重点，解决登录界面）
- `ue_create_widget_blueprint` — 配方内**多策略**创建带根的 WBP，按序尝试：
  1. `WidgetBlueprintFactory` 创建 → `bp.get_editor_property("WidgetTree")` 取树 → `tree.construct_widget(CanvasPanel)` 设根。
  2. 若 WidgetTree binding 缺失 → `unreal.load_module("UMGEditor")` 后重试。
  3. 若仍失败 → 走 RC `set_property`（依赖已配置的 `bIgnoreProtectedCheck`）设 RootWidget。
  4. 全失败 → 返回结构化 `unsupported`，明确指向 Layer 3 插件，**不无限重试**。
  - 每种策略的成败写入返回的 `strategiesTried`，便于诊断。
- `ue_add_widget_to_blueprint` — 往 WBP 加 Button/Text/Image/EditableTextBox 等子控件并设布局（slot 属性）。
- `ue_set_widget_property` — 设控件文本/颜色/位置等。

> 蓝图**图表节点连线**（add node/connect pin）纯 Python 极痛苦，归入 Layer 3。

---

## Layer 3 — 引擎内 helper 插件（仅设计，待 greenlight）
- 极小 UE 插件，暴露少量 `UFUNCTION`（设 protected 成员、construct_widget、连蓝图节点引脚），经 RC `call_function` 调用。
- 解决 Layer 2 配方在精简构建上仍搞不定的硬骨头。
- **张力**：与"for-all-versions 跨 4.25→5.8 单二进制"卖点冲突（每版本需编译插件）。建议作为**可选增强包**，检测到插件存在时点亮额外 Capability，缺失则维持现有降级。
- 设计产物：`.uplugin` 模板 + 1 个 module + N 个 UFUNCTION 签名清单 + 跨版本编译说明。本阶段产出设计文档，不写引擎代码。

---

## 文件改动清单
- `include/.../tool_registry.hpp` — 声明新 `register_*()` 方法。
- `src/tool_registry.cpp` — `run_python_recipe()` 助手 + 新分组实现 + 配方脚本常量；`register_builtins()` 末尾追加调用。
- `include/.../capability_registry.hpp` + `src/capability_registry.cpp` — 新增 `AssetTools` capability + probe + `to_string`/`describe`。
- `tests/test_main.cpp` — tools/list 增加新工具名 spot-check；新工具的降级路径断言（Python 不可用→unsupported）。
- `scripts/selftest_*.py` — 新增 live 自测：建材质→建蓝图→建 WBP→加按钮→编译保存，对真实编辑器跑通。
- `Layer 3` 设计文档：`.omc/plans/layer3-inengine-plugin-design.md`（仅设计）。
- `README.md` — 工具计数与分类更新。

## 实施顺序
1. 基础设施：`run_python_recipe()` + `AssetTools` capability + 单测。
2. Layer 1 全部工具 + 单测降级断言。
3. Layer 2：`ue_create_widget_blueprint`（多策略）优先，再补其余 authoring 工具。
4. live selftest 脚本跑通真实编辑器（至少 ue53 测试工程）。
5. Layer 3：产出设计文档，等 greenlight。

## 验证
- `cmake --build build --config Release` 通过。
- `ctest`（test_main）全绿——含新工具 tools/list 与降级断言。
- 对运行中的 ue53 编辑器跑 selftest，确认 create_widget_blueprint 至少一条策略成功建出带根 WBP 并加上按钮。

## 风险与诚实边界
- UMG 在精简 Python 构建上可能多策略全败——届时返回 `unsupported` 并指向 Layer 3，而非假装成功。
- `bIgnoreProtectedCheck` 需编辑器重启才生效；selftest 前需确认测试工程已重启。
- `/remote/batch` 仅 4.26+；`ue_batch_spawn_actors` 在 4.25 自动降级。
