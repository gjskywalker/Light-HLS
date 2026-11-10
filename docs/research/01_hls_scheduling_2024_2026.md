# HLS 調度：2024–2026 代表工作與 Light-HLS 的研究切入點

整理日期與檢索截止：**2026-11-10（香港時間）**。供 2026-11-11 review。本文將「近三年」解讀為 2024、2025、2026 三個曆年；2026 尚未結束。

**核心判斷：還有改進空間，但不能僅以「把 static 與 dynamic 結合」作為新意。** 近年的進展更具體：找出哪些依賴必須在執行時處理、消除多餘握手、讓資源共享與動態執行共存，以及改變迴圈之間可重疊的範圍。對目前 Light-HLS，先研究可驗證的靜態調度啟發式及其資源／存儲聯合決策，比立即重建完整動態後端更容易得到可歸因的結果。這是本文的研究建議，尚非實驗結論。

## 1. 先釐清分類與比較層級

| 類型 | 哪些決策在何時作出 | 主要收益 | 必須計入的代價 |
|---|---|---|---|
| Static scheduling | 編譯時確定相對啟動時刻、控制狀態或模調度；執行時仍可有條件分支與停頓 | 容易做時序控制、運算鏈接與資源共享 | 對不確定依賴和長短路徑可能較保守 |
| Dynamic scheduling | 硬體依資料／控制 token、資源可用性等條件決定啟動 | 可利用執行時才知道的獨立性，適合部分不規則程式 | 握手、緩衝、仲裁、記憶體消歧與死鎖處理成本 |
| Hybrid scheduling | 在不同區域、粒度或依賴上組合兩者 | 僅為真正有用的動態性付費 | 分區、邊界協議和成本模型會影響整體收益 |

上述分類描述的是**所生成硬體的調度方式**。用 GNN、RL 或 LLM 在編譯時搜尋排程，生成的仍可能是純靜態硬體；有 FIFO 和 DATAFLOW 也不能直接等同於逐運算動態調度。基本分類與代價可參照 [DASS 正式論文](https://doi.org/10.1109/TCAD.2021.3065902)。

另外，operation scheduling、loop transformation、task scheduling、resource binding 是相互作用但不同的問題。下面明確標出層級，避免把一個較完整的編譯流程改善誤認為單一 scheduler 的改善。

## 2. 近三年值得優先讀的七篇

選取依據是問題相關性、PLDI／ASPLOS／FPGA／ICCAD 等主要研究社群的正式發表，以及可核驗的方法或實驗材料。這是**代表性閱讀清單，不是引用量排名或系統性綜述**；較新的 2026 工作尤其不能斷言已有長期影響力。

| 論文、年份與狀態 | 調度類型／層級 | 方法與價值 | 閱讀時應保留的界線 |
|---|---|---|---|
| **[Hyperblock Scheduling for Verified High-Level Synthesis](https://yannherklotz.com/papers/pldi24_hsvhls.pdf)**；Yann Herklotz、John Wickerson；**PLDI 2024 / PACMPL 8(PLDI), Article 225**，已正式發表；[DOI](https://doi.org/10.1145/3656455) | Static；跨基本區塊的運算調度 | 對帶 predicate 的 hyperblock 排程並支援 operation chaining；以 Coq 驗證的 validator 檢查每份排程。PolyBench/C 上，相對原 Vericert，作者報告硬體平均加速 2.1×。 | 重點是讓強化 scheduler 仍具正確性保證；不能把對原 Vericert 的提升理解成普遍勝過商用 HLS。對 agent 自動修改 scheduler 特別有方法論價值。 |
| **[Stream-HLS: Towards Automatic Dataflow Acceleration](https://arxiv.org/html/2501.09118v1)**；Suhail Basalama、Jason Cong；**FPGA 2025**，已正式發表；[DOI](https://doi.org/10.1145/3706628.3708878) | 編譯時全域優化；多 kernel 的 loop／task 層級 | MLIR 框架與解析模型，使用 MINLP 協同考慮迴圈順序、節點流水、圖層流水和並行度，生成含 FIFO 的 dataflow 架構。說明單節點最優不代表整個應用最優。 | 不能只因有串流便歸類為 Dynamatic 式動態調度；它主要是上層架構／調度搜尋，並非替換任意 operation scheduler 的通用演算法。 |
| **[R-HLS: An IR for Dynamic High-Level Synthesis and Memory Disambiguation based on Regions and State Edges](https://www.sjalander.com/research/pdf/sjalander-iccad2024-rhls.pdf)**；David Metz 等；**ICCAD 2024**，已正式發表；[DOI](https://doi.org/10.1145/3676536.3676671) | Dynamic；IR 與記憶體依賴表達 | 以 RVSDG 的 regions／state edges 表達全域資料流，並分散處理記憶體消歧，減少控制流 IR 重建跨區塊並行性的負擔。 | 這是 IR 與硬體生成方法的共同改變；報告的改善不能全部歸因於一個排程優先級公式。適合思考「搜尋空間是否被 IR 限制」。 |
| **[Suppressing Spurious Dynamism of Dataflow Circuits via Latency and Occupancy Balancing](https://dynamo.ethz.ch/wp-content/uploads/sites/22/2024/04/Xu_FPGA24_SuppressingSpuriousDynamism.pdf)**；Jiahui Xu、Lana Josipović；**FPGA 2024**，已正式發表；[DOI](https://doi.org/10.1145/3626202.3637570) | Dynamic 電路的靜態化／簡化；控制與緩衝層級 | 用線性規劃平衡 latency 與 token occupancy，減少虛假停頓，進而暴露可移除握手邏輯的區域。它直接挑戰「動態電路一定需要到處握手」的直覺。 | 與 DASS 的整塊 static islands 路徑不同；不能把所有動態控制都移除。收益取決於電路是否具有可利用的規律性與選定的面積／吞吐目標。 |
| **[CRUSH: A Credit-Based Approach for Functional Unit Sharing in Dynamically Scheduled HLS](https://dynamo.ethz.ch/wp-content/uploads/2025/06/Xu_ASPLOS25_CRUSH.pdf)**；Jiahui Xu、Lana Josipović；**ASPLOS 2025, Vol. 1, pp. 249–263**，已正式發表；[DOI](https://doi.org/10.1145/3669940.3707273) | Dynamic；資源共享與執行控制 | 用 credit 解耦共享運算器的資源依賴，避免共享引入的死鎖，並保留亂序存取共享單元的機會。相對 prior in-order sharing，作者報告平均 DSP 減少 12%、FF 減少 15%。 | 這不意味「任意共享都不會死鎖」；其保證有電路與協議假設。共享 wrapper 自身也有成本，應同時看執行時間與資源。 |
| **[Dynamic Loop Fusion in High-Level Synthesis](https://arxiv.org/html/2501.14631v1)**；Robert Szafarczyk、Syed Waqar Nabi、Wim Vanderbauwhede；**FPGA 2025, pp. 211–222**，已正式發表；[DOI](https://doi.org/10.1145/3706628.3708871) | 編譯器／硬體協同；靜態分析加動態記憶體消歧 | 利用地址單調性與 program-order schedule，讓有不確定記憶體依賴的 sibling loops 重疊執行。證明研究空間不只在單個 loop 內的 II。 | 關鍵條件是內層迴圈地址單調不遞減；不是任意 irregular loops 的通解。作者的 14×／4× 平均加速分別對其 static／dynamic baselines，不能直接外推至 Light-HLS。 |
| **[Out with LSQs: Custom Circuits for Memory Access Reordering in Dynamic HLS](https://www.epfl.ch/labs/lap/wp-content/uploads/2026/03/PirayadiFeb26-Out-with-LSQs-Custom-Circuits-for-Memory-Access-Reordering-in-Dynamic-HLS-FPGA26.pdf)**；Rouzbeh Pirayadi、Ayatallah Elakhras、Mirjana Stojilović、Paolo Ienne；**FPGA 2026, pp. 92–102**，已正式發表；[DOI](https://doi.org/10.1145/3748173.3779204) | Dynamic；依賴專用的記憶體執行控制 | 依靜態分析後真正剩餘的依賴生成定制控制電路，取代過於通用的 load-store queue。相對優化過的 LSQ，作者報告相近執行時間下平均資源減少 37%。 | 是記憶體消歧機制的改進，並不自動解決所有調度問題。地址追蹤窗口、條件執行及前進性仍需處理；2026 工作的後續影響尚待觀察。 |

**第八篇，直接對應 hybrid 的補讀：** Yuhan She、Yanlong Huang、Jierui Liu、Ray C. C. Cheung、Hong Yan，**[A Source-to-Source Compiler to Enable Hybrid Scheduling for High-Level Synthesis](https://www.mdpi.com/2079-9292/14/23/4578)**，*Electronics* 14(23):4578，2025-11-22，已正式發表，[DOI](https://doi.org/10.3390/electronics14234578)。它將動態性暴露於 source 層，再使用現有 static HLS 工具，對「保留成熟後端，透過前端變換實現混合執行」很有參考價值。將其列為相關補充，不與前七篇的社群代表性等量看待；現有檢索不足以支持其已具同等影響力。

## 3. 必須知道、但不應冒充近三年的奠基工作

1. Jianyi Cheng、Lana Josipović、George A. Constantinides、Paolo Ienne、John Wickerson，**[Combining Dynamic & Static Scheduling in High-level Synthesis](https://jianyicheng.github.io/assets/pdf/ChengFPGA20.pdf)**，FPGA **2020**；擴展版 **[DASS](https://doi.org/10.1109/TCAD.2021.3065902)** online **2021**、TCAD 卷期 **2022**。在動態外圍中放入 static islands；因此「混合」本身早已不是新問題。
2. Robert Szafarczyk、Syed Waqar Nabi、Wim Vanderbauwhede，**[Compiler Discovered Dynamic Scheduling of Irregular Code in High-Level Synthesis](https://eprints.gla.ac.uk/303044/)**，FPL **2023**，[DOI](https://doi.org/10.1109/FPL60245.2023.00009)。反過來從 static HLS 出發，選擇性引入動態基本區塊、迴圈和記憶體操作；對 Light-HLS 的現有架構比全面動態化更接近。雖然學校頁面在 2026 更新，它仍是 2023 論文。

## 4. 是否存在「更好的 scheduling algorithm」？

**在明確的目標與工作負載下可以更好；目前證據不支持一個同時全面支配 static、dynamic 和 hybrid 的單一方法。** 單純減少 cycle count 可能拉長 critical path；增加並行性可能增加 DSP、RAM、仲裁與 routing 負擔；對 profile 的期望收益也可能在資料分布改變時消失。上述論文的 baseline、平台、記憶體系統及優化範圍並不一致，不能把各自 speedup 排成排行榜。

綜合前述工作，值得研究的決策空間是：

> 對每一個區域／依賴，決定多少順序可在編譯時固定、哪些條件必須在執行時判定，並一併決定資源共享、緩衝、bank 與物理時序代價。

這是對文獻的綜合推論，並非已證實的新演算法。可分成三條路：

若進一步考慮「執行階段改變時換排程／配置」，也已有直接相關工作：Mathias Bouilloud、Lana Josipović、Wayne Luk 的 **[Resource and Phase Awareness for Dynamically Scheduled High-Level Synthesis](https://dynamo.ethz.ch/wp-content/uploads/2025/06/Bouilloud_HEART25_ResourceAndPhaseAwareness.pdf)**，**HEART 2025, pp. 14–24**，[DOI](https://doi.org/10.1145/3728179.3728194)。它對 buffer 優化加入資源上限，並為不同 phases 生成配置、探討執行時重配置。其 phase 偵測與頻率資訊是給定假設；因此「phase-aware」這個方向本身也不能當作新穎性。

| 路徑 | 可檢驗問題 | 已有工作的重疊與風險 |
|---|---|---|
| **A. 改善靜態 heuristic，聯合考慮 binding／storage／banks** | 對相同合法操作集合，更好的 ready-list 優先級、模排程順序或有限回溯，能否在保留 II 的同時減少暫存、mux 與實際執行時間？ | 屬於成熟研究方向；需提出可重複的決策規則與跨程式證據。只換一個權重或只調 pragma，貢獻可能不足。 |
| **B. 靜態主幹＋依賴專用的局部動態控制** | 能否只對無法靜態消除且確實限制吞吐的依賴，生成小型執行時檢查，而不讓整個 datapath 都握手化？ | 與 DASS、FPL23、FPGA26 LSQ 替代機制高度接近；必須比對分區粒度、生成方法、成本模型與正確性條件，不能直接宣稱新。 |
| **C. 以 profile 選擇有保護的特化排程** | 對常見分支／無 alias 情況使用快路徑，條件不成立時安全切回保守排程，能否抵銷 guard 與重複控制成本？ | Guard、speculation、profile-guided 優化都是既有概念。新意需來自可證明的切換協議、特定可利用結構或更好的搜尋策略；profile 不得用來刪除真依賴。 |

我建議 **先 A，再以 B 為中期方向**。A 可以較快回答「agent 是否真的找到泛化的排程規則」；B 要求新增協議與後端能力，若一開始同時改 scheduler、IR 和 RTL，較難判定收益來源。C 需要足夠真實的輸入分布，暫不作第一個實驗。

## 5. 對目前 Light-HLS 的具體落點

本節根據當前本地程式與設計文件，不代表已實作下列研究提案。

- [後端設計說明](../RTL_BACKEND_DESIGN.md)描述普通路徑為基本區塊內 ASAP 調度，跨區塊值註冊；[HI_OperationScheduling.cc](../../Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc)還包含單區塊 loop pipeline、依賴／記憶體限制及模端口保留。這是一個足以開始做靜態 scheduler 對照的基礎。
- [HI_ResourceBinding](../../Light_HLS/Implementations/HI_ResourceBinding/HI_ResourceBinding.cc)、[HI_BackendDependenceAnalysis](../../Light_HLS/Implementations/HI_BackendDependenceAnalysis/HI_BackendDependenceAnalysis.cc)與[HI_BackendValidation](../../Light_HLS/Implementations/HI_BackendValidation/HI_BackendValidation.cc)提供可分離的決策與檢查位置。新增候選策略時，先維持同一套依賴、接口與合法性契約，讓差異可歸因於排程決策。
- 現有 DATAFLOW／FIFO 後端可提供任務層級實驗材料，但在未檢查其逐操作執行協議之前，不應把它當作已具備完整 Dynamatic 類 dynamic scheduling 的證據。

**第一個可被否證的研究假說：** 在保持現有 RTL 語義與支持範圍的條件下，agent 產生的排程 heuristic 能利用 recurrence criticality、bank 衝突和 value lifetime，對未參與搜尋的程式改善「實際執行時間—資源」Pareto 前緣，而不只改善估計 latency。

最小實驗約定如下：

1. **凍結問題。** 先選普通靜態區塊或現有單區塊 pipeline 其中一類；固定 LLVM 優化流程、board、時鐘、運算器 latency、接口與存儲配置。以現有 scheduler、常見 critical-path／mobility list scheduling 和小圖上的精確求解器作對照。
2. **隔離搜尋與評估。** 按程式家族切分 train／validation／test，保留不同 trip count、bank／port 數、分支條件與輸入分布；用相同評估次數和時間預算比較 agent、隨機搜尋及人工 heuristic。
3. **先檢查合法性再計分。** 檢查 SSA／記憶體／跨迭代依賴、端口與資源衝突、可觀察副作用順序；做 C／IR／RTL 差分測試及適用範圍內的形式化驗證。若加入動態控制，另需檢查前進性與死鎖，單次模擬通過不夠。
4. **保留可實測結果。** 同時記錄 achieved II、transaction cycle count、綜合／實作時序和 LUT／FF／DSP／BRAM；以明確接口事件定義開始與完成。執行時間應以實際工作頻率計算，不能用目標頻率替代未達成的時序。
5. **設置失敗判準。** 如果收益只存在搜尋用 benchmarks、只改善估計器，或等預算非 agent 搜尋也有同樣收益，就不能聲稱發現更好的通用 scheduling algorithm。留下失敗例與消融結果，它們能指向下一輪需要改變的規則。

## 6. 明天 review 時建議先作的三個判斷

1. **研究對象：** 第一階段是否只做現有靜態／模排程 heuristic，而把新增動態硬體留到下一階段？
2. **成功標準：** 主要追求固定資源約束下的實際執行時間，還是固定吞吐下的資源減少？用明確約束及 Pareto 前緣，比任意加權總分更容易解釋。
3. **論文貢獻：** 要產出「agent 輔助搜尋流程」，還是「可獨立執行、可說明且跨程式有效的新排程規則」？後者對使用者最初提出的演算法研究問題更直接。

## 7. 檢索與證據範圍

採用 academic-research-suite 的 literature-review 路徑，依序做候選檢索、出版資訊核對及跨工作綜合；不是 PRISMA 系統性綜述。查詢包括 `HLS static dynamic hybrid scheduling 2024 2025 2026`、`Dynamatic functional unit sharing`、`dynamic loop fusion`、`HLS memory disambiguation` 以及各篇精確題名；再追查作者／實驗室正式頁面、出版社紀錄與論文全文。納入 2024–2026 正式研究論文，早期工作另列；排除影片串流的同名 HLS、FPGA 雲端作業調度，以及僅做 pragma 推薦卻無調度方法關係的工作。

本清單在 dynamic 路徑上較集中於 ETH／EPFL 的 Dynamatic 研究脈絡，已補入 Glasgow、UCLA 與 R-HLS 路線，但仍有學派與工具偏向。數值均是作者在各自實驗設定中的報告，本文未重現外部論文實驗；新穎性只是本次有限檢索下的研究判斷。部分 ACM DOI 直接開啟受存取限制，已以作者公開全文或大學典藏交叉核對，不將存取限制當作文獻不存在。2026 的補充工作與後續引用仍值得在正式立題前再檢索一次。
