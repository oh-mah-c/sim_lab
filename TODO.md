# **SUNNYWORLD – OPEN-ENDED EVOLUTION ENGINE**



Mục tiêu C – Open-ended Evolution

Pure Code Architecture



Tôi sẽ thiết kế Sunnyworld theo hướng một **Causal World Simulation Engine**: engine mô phỏng một thế giới có những quy luật khách quan, trong đó sinh vật, con người và xã hội tự thay đổi thông qua tương tác, khám phá, thử nghiệm, thích nghi và tích lũy tri thức.

Đây không đơn thuần là một hệ thống NPC thông minh hơn.

Đây là một kiến trúc trong đó:

- Thế giới tồn tại độc lập với NPC.
- Các quy luật tự nhiên tồn tại trước khi được khám phá.
- NPC không được cấp sẵn kiến thức về thế giới.
- Công nghệ không cần có sẵn trong cây công nghệ.
- Sinh vật không bị giới hạn trong danh sách loài cố định.
- Xã hội không cần có sẵn danh sách hình thái tổ chức.
- Mọi thay đổi phải có nguyên nhân, điều kiện và kết quả.
- Những kết quả mới có thể tạo ra những khả năng mới.

Tôi muốn đặt ra một nguyên tắc xuyên suốt:

> **Code định nghĩa cách thế giới vận hành. Data định nghĩa thế giới có những gì. NPC khám phá thế giới. Không có code nào quyết định trước lịch sử của thế giới sẽ diễn ra như thế nào.**

Có một giới hạn khoa học cần thống nhất ngay từ đầu: không engine nào có thể tự suy ra toàn bộ vật lý, hóa học và sinh học từ con số 0. Engine cần được cung cấp những quy luật nền tảng và các mô hình có khả năng biểu diễn chúng. Nhưng từ những nền tảng đó, nó có thể tạo ra rất nhiều kết quả và phương pháp chưa từng được lập trình riêng.

Đây là cách tôi đề xuất xây dựng.

# **PHẦN I – KIẾN TRÚC TỔNG THỂ**

## **1. Cấu trúc các tầng của Sunnyworld**

# **SUNNYWORLD ENGINE**

Core Runtime · Deterministic Simulation

**LAYER 1 – WORLD FOUNDATION**

World State

Entity & Ontology

Properties & Materials

Natural Resources & Deposits

Space & Environment

**LAYER 2 – NATURAL LAW SYSTEM**

Physics Engine

Chemistry Engine

Biology Engine

Ecology Engine

**LAYER 3 – CAUSALITY & COMPOSITION**

Interaction Resolver

Causal Engine

Composition Engine

Constraint Solver

**LAYER 4 – INTELLIGENCE & DISCOVERY**

Perception

Memory & Belief

Hypothesis Generator

Experimentation

**LAYER 5 – EMERGENT EVOLUTION**

Technology Evolution

Biological Evolution

Social Evolution

Cultural Evolution

**WORLD HISTORY & KNOWLEDGE**

Causal Records · Discoveries · Provenance · Social Memory

Các tầng tương tác hai chiều thông qua những giao diện xác định, không phải một pipeline một chiều.

Tôi đề xuất chia thành 12 module kỹ thuật chính:

| Module             | Nhiệm vụ                             |
| ------------------ | ------------------------------------ |
| World Core         | Quản lý trạng thái thế giới          |
| Entity System      | Quản lý mọi thực thể                 |
| Property System    | Đặc tính của vật chất và sinh vật    |
| Natural Resource System | Occurrence, Deposit, Extraction, Processing |
| Natural Laws       | Các quy luật tự nhiên                |
| Interaction System | Tương tác giữa thực thể              |
| Causal Engine      | Giải quyết nhân quả                  |
| Composition Engine | Tạo cấu trúc và tổ hợp mới           |
| Agent Intelligence | Nhận thức và ra quyết định           |
| Discovery Engine   | Khám phá tri thức mới                |
| Evolution Engine   | Tiến hóa sinh học và xã hội          |
| Knowledge System   | Lưu trữ và truyền bá kiến thức       |
| World History      | Ghi lại lịch sử và nguồn gốc sự kiện |

Điểm quan trọng: các module này không được phụ thuộc vòng tròn vào nhau. Chúng phải giao tiếp thông qua những interface và sự kiện được định nghĩa rõ ràng.

## **1.1. Ba thành phần cốt lõi phải độc lập nhưng tương tác**

Sunnyworld là **World Simulation Engine – bộ quy luật vận hành thế giới**, không phải game mô phỏng sinh tồn thông thường. Mọi thứ tồn tại, tương tác, biến đổi và tiến hóa thông qua nguyên nhân và kết quả.

> Engine không quy định NPC phải làm gì trong từng tình huống. Engine quy định thế giới vận hành như thế nào, NPC có thể làm gì trong thế giới đó, và điều gì xảy ra khi chúng hành động.

Ví dụ: NPC muốn có gỗ — engine không viết sẵn nhiệm vụ `CollectWood`; NPC phải tự tìm ra con đường đạt mục tiêu.

| Thành phần | Vai trò |
|---|---|
| **WORLD RULES (Engine)** | Quy luật vật chất, không gian, thời gian, năng lượng, sinh học, địa chất, tương tác, nhân quả |
| **WORLD CONTENT (Data)** | Núi, sông, cây, động vật, khoáng sản, khí hậu, con người, vật liệu, đặc tính, trạng thái |
| **AGENT INTELLIGENCE (NPC)** | Nhu cầu, mục tiêu, nhận thức, ký ức, kiến thức, suy luận, thử nghiệm, học hỏi |

Engine biết cây có cấu trúc vật chất và chịu lực; Data xác định một cây sống tại vị trí A; NPC nhìn thấy cây và tự quyết định có thử chặt hay không. **Engine không được đưa kiến thức về cây vào đầu NPC.**

Mười năng lực của engine (không phải mười thư viện phải tách rời — có thể tổ chức thành ít module hơn):

1. World Foundation – không gian, thời gian, thực thể, môi trường, trạng thái.
2. Natural Law System – cơ học, nhiệt động, hóa học, địa chất, thủy văn, sinh học, sinh thái.
3. Ecosystem & Evolution – sinh trưởng, sinh sản, di truyền, đột biến, cạnh tranh, chọn lọc, tuyệt chủng.
4. Resource & Material System – phân bố, phát hiện, khai thác, chế biến, chế tạo, cạn kiệt.
5. Perception & Knowledge – quan sát, ghi nhớ, phân loại, suy luận, niềm tin, giả thuyết, truyền đạt.
6. Action & Interaction – đi, chạy, kéo, đẩy, cắt, đào, nhặt, mang, chế tạo, xây dựng, giao tiếp, chiến đấu — mọi hành động có điều kiện và kết quả nhân quả.
7. Agent Intelligence – nhu cầu, động lực, mục tiêu, lập kế hoạch, đánh giá rủi ro.
8. Social & Cultural Evolution – gia đình, hợp tác, phân công, trao đổi, quyền lực, luật lệ, tôn giáo, ngôn ngữ.
9. Causality & World History – nguyên nhân, điều kiện, kết quả, hậu quả dây chuyền, nguồn gốc.
10. Simulation & Consistency – lập lịch, sự kiện, trạng thái, bảo toàn tài nguyên, xung đột, deterministic replay, hiệu năng.

# **PHẦN II – THIẾT KẾ CHI TIẾT CÁC MODULE**

## **2. World Generation Engine – Thế giới được sinh ra từ quá trình, không phải danh sách tài nguyên rải ngẫu nhiên**

World Generation phải tạo ra một thế giới có địa lý, địa chất, khí hậu và sinh thái **nhất quán** trước khi NPC xuất hiện. Không thể chỉ tạo một danh sách tài nguyên rồi rải ngẫu nhiên lên bản đồ — nếu làm vậy Sunnyworld có thể có quặng vàng trên cây, rừng mọc giữa sa mạc, hay sông chảy ngược lên núi.

**Nguyên tắc nền tảng:**

> Tài nguyên phải là hệ quả của lịch sử hình thành thế giới, không phải những vật phẩm được đặt ngẫu nhiên lên bản đồ.

Phải mô phỏng thế giới từ địa lý và các quá trình tự nhiên; tài nguyên mới xuất hiện theo điều kiện phù hợp.

### **2.1. Thế giới được tạo theo nhiều tầng phụ thuộc**

Thứ tự phụ thuộc để tạo thế giới ban đầu (không phải chuỗi tuyến tính bất biến — thủy văn tác động ngược địa hình qua xói mòn; sinh vật tác động đất; hoạt động địa chất có thể đổi nguồn nước):

1. **Geological Foundation** – đá nền, kiến tạo, đứt gãy, núi, bồn trũng, trầm tích.
2. **Terrain Formation** – độ cao, độ dốc, thung lũng, đồng bằng, hang động, lòng sông.
3. **Climate & Hydrology** – mưa, nhiệt độ, gió, nước ngầm, dòng chảy, hồ, sông.
4. **Ecology** – rừng, đồng cỏ, cây cối, động vật, đất, thảm thực vật.
5. **Resource Distribution** – mỏ khoáng sản, quặng, gỗ, đất sét, động vật, thực vật, nước.

### **2.2. Mỗi loại tài nguyên có điều kiện địa lý riêng**

Không phải tài nguyên nào cũng xuất hiện theo cùng quy tắc, và cùng một tài nguyên có thể có nhiều kiểu phân bố — không được quy định đơn giản `gold = mountain` hay `clay = river`.

Ví dụ các điều kiện hình thành:

- **Vàng:** mạch thạch anh do hoạt động thủy nhiệt; trầm tích sông do xói mòn và phân loại vật liệu; sâu trong đá hoặc lộ ở sườn núi. Không tự nhiên mọc trên cây.
- **Đồng:** hệ thống địa chất và khoáng hóa nhất định; đá magma, đá biến chất, đá trầm tích; mạch khoáng, thân quặng, vùng phân tán.
- **Than đá:** từ vật chất thực vật tích tụ; cần môi trường trầm tích phù hợp và biến đổi địa chất lâu dài; thường thành vỉa trong đá trầm tích.
- **Đất sét:** phong hóa hóa học của đá; vận chuyển và lắng đọng bởi nước; vùng trầm tích, bãi bồi, hồ cổ.
- **Gỗ/thực vật:** phụ thuộc khí hậu, mưa, nhiệt độ, đất, độ cao — mỗi loài cây cần điều kiện sinh trưởng riêng; rừng không thể xuất hiện chỉ vì một ô được gán `forest`.
- **Nước:** sông bắt nguồn từ vùng cao theo trọng lực; nước ngầm chịu ảnh hưởng địa chất, độ thấm, mực nước.

### **2.3. Cấu trúc không gian ba lớp**

Thế giới không thể chỉ là bản đồ 2D với mỗi ô chứa danh sách tài nguyên — địa chất hợp lý đòi hỏi phân biệt ít nhất ba dạng không gian:

- **A. Surface Layer** – núi, đồi, đồng bằng, thung lũng; rừng, hồ, sông, đất, thảm thực vật; tài nguyên tiếp xúc trực tiếp.
- **B. Subsurface Layer** – tầng đất và đá; mạch khoáng, vỉa than, thân quặng; hang động, nước ngầm; độ sâu và cấu trúc địa chất.
- **C. Atmospheric Layer** – nhiệt độ, mưa, gió, độ ẩm; yếu tố ảnh hưởng phong hóa, thực vật, động vật.

Không nhất thiết dùng voxel 3D toàn bộ. Có thể dùng: **heightmap** cho địa hình, **geological strata** cho cấu trúc dưới đất, **spatial fields** cho khí hậu/môi trường, **resource deposits** cho vùng phân bố, **entity/instance** cho vật thể cụ thể (cây, đá, động vật). Cách này cân bằng độ chân thực và hiệu năng.

### **2.4. Mô phỏng lịch sử hình thành, không chỉ đặt tài nguyên**

Tạo mỏ vàng không phải `if (terrain == MOUNTAIN) spawnResource(GOLD)`. Phải có chuỗi quá trình địa chất, ví dụ:

1. Hoạt động địa chất thích hợp.
2. Chất lỏng nhiệt dịch di chuyển qua khe nứt.
3. Khoáng chất vận chuyển và kết tủa trong điều kiện phù hợp.
4. Hình thành vùng khoáng hóa.
5. Nâng lên và xói mòn làm lộ một phần mạch khoáng.
6. Sông suối vận chuyển vàng, tập trung vàng sa khoáng ở một số vị trí.

Vị trí mỏ vàng là **kết quả của lịch sử địa chất**. NPC có thể phát hiện mạch vàng trên sườn núi hoặc vàng sa khoáng trong lòng suối, nhưng không nhất thiết hiểu nguồn gốc địa chất.

### **2.5. Các module World Generation**

| Module | Nhiệm vụ |
|---|---|
| Geological Generator | Nền địa chất, đá nền, cấu trúc kiến tạo |
| Terrain Generator | Độ cao, núi, sông, thung lũng |
| Hydrology Generator | Lưu vực, nước mặt, nước ngầm |
| Climate Generator | Nhiệt độ, mưa, gió, khí hậu |
| Soil Generator | Đất theo đá mẹ, khí hậu, sinh học |
| Geological Process Simulator | Khoáng hóa, trầm tích, phong hóa |
| Resource Distribution Generator | Vùng phân bố tài nguyên |
| Ecology Generator | Hệ sinh thái, thực vật, động vật |
| World Validation | Kiểm tra tính nhất quán tự nhiên |

Các module trao đổi dữ liệu với nhau: Ecology cần khí hậu và đất; Resource Distribution cần địa chất; Hydrology cần địa hình và độ thấm của đất đá.

### **2.6. World Validation – kiểm tra thế giới sau khi tạo**

| Kiểm tra | Kết quả mong đợi |
|---|---|
| Quặng vàng trên cây | Không hợp lệ |
| Mạch quặng trong đá phù hợp | Có thể hợp lệ |
| Vàng sa khoáng ở bãi bồi | Có thể hợp lệ |
| Sông chảy ngược lên dốc | Không hợp lệ |
| Rừng ở sa mạc cực khô | Cần điều kiện đặc biệt |
| Vỉa than trong tầng đá trầm tích phù hợp | Có thể hợp lệ |

Không kiểm tra bằng danh sách quy tắc cứng — phải kiểm tra điều kiện vật lý, địa chất, khí hậu và quan hệ nhân quả. Ngoại lệ hợp lý vẫn phải được chấp nhận.

### **2.7. Bốn nguyên tắc thiết kế**

1. **Geography before Resources** – tạo địa lý trước; tài nguyên xuất hiện theo điều kiện địa lý.
2. **Natural Processes before Distribution** – tài nguyên hình thành qua quá trình tự nhiên, không rải ngẫu nhiên.
3. **World Truth before NPC Knowledge** – tài nguyên tồn tại độc lập; NPC phải tự khám phá.
4. **Extraction before Usability** – tìm thấy tài nguyên không có nghĩa đã khai thác hay chế biến được.

Lưu ý: không cần mô phỏng hàng triệu năm địa chất theo từng giây. World Generator tạo trạng thái hiện tại từ một lịch sử địa chất được mô hình hóa bằng các quá trình và quy tắc có kiểm soát; trong quá trình chạy, các quá trình tự nhiên tiếp tục biến đổi thế giới ở mức độ phù hợp.

**Mục tiêu cuối:** thêm một loại khoáng sản mới vào dữ liệu chỉ cần mô tả thành phần, đặc tính và điều kiện hình thành — engine dùng các quy luật địa chất đã hỗ trợ để xác định nơi nó có thể hình thành, không cần tự tay đặt từng mỏ.

### **2.8. Seed → World Map – hệ thống sinh thế giới deterministic**

Phần Seed → World Map là một **subsystem riêng** trong World Generation, không phải một con số đưa vào hàm random để rải núi, sông, cây và mỏ.

**Nguyên tắc chốt:**

> Cùng một World Seed + cùng phiên bản thuật toán + cùng cấu hình thế giới = cùng một World Map.

Seed **không quyết định trực tiếp từng vật thể**. Nó khởi tạo các điều kiện ban đầu và các quá trình sinh thế giới; các hệ thống địa chất, khí hậu, thủy văn, sinh thái và tài nguyên sau đó tạo ra bản đồ nhất quán.

**Pipeline:**

```
World Seed
  → World Generation Configuration (kích thước, tham số địa lý, generator version)
  → Independent Generation Domains (Geology, Terrain, Climate, Hydrology,
     Soil, Ecology, Resources, Subsurface)
  → Generated World State
```

Các domain có **thứ tự phụ thuộc** (địa chất/địa hình → thủy văn → đất → sinh thái/tài nguyên) nhưng không được phép tự random độc lập rồi ghép lại. Pipeline không phải tuyến tính bất biến — các domain có thể trao đổi trường dữ liệu qua lại theo §2.1.

**A. Seed phải có nhiều nhánh độc lập**

Không dùng một RNG duy nhất chạy liên tục cho toàn bộ World Generation:

```
geologySeed   = derive(WorldSeed, "geology")
terrainSeed   = derive(WorldSeed, "terrain")
climateSeed   = derive(WorldSeed, "climate")
waterSeed     = derive(WorldSeed, "hydrology")
ecologySeed   = derive(WorldSeed, "ecology")
resourceSeed  = derive(WorldSeed, "resources")
```

Lợi ích bắt buộc:

- Thay đổi thuật toán sinh cây **không** làm thay đổi địa chất.
- Bổ sung tài nguyên mới **không** làm biến dạng sông núi.
- Chạy song song các hệ thống mà không phụ thuộc thứ tự thực thi.
- Kiểm thử được từng domain riêng.

**B. Coordinate-based determinism**

Với thế giới mở rộng theo chunk, kết quả phải sinh theo **tọa độ toàn cục**, không phụ thuộc chunk nào được tạo trước:

```
value = noise(worldSeed, globalX, globalY);
```

Không được để kết quả phụ thuộc thứ tự tạo chunk. FastNoise2 (seed + seed offset theo node) là lựa chọn đáng cân nhắc cho lớp noise — nhưng chỉ là noise layer, không phải toàn bộ World Generator.

**C. Hai quy mô bản đồ**

- **Global World Map** – lục địa/đại dương, dãy núi, lưu vực sông lớn, vùng khí hậu, vùng địa chất, phân bố sinh thái–tài nguyên quy mô lớn. Có thể là trường địa lý liên tục theo tọa độ toàn cục, **không nhất thiết** là một mảng dữ liệu duy nhất.
- **Regional/Local Map** – địa hình chi tiết, tầng đất/địa chất dưới đất, suối/sông nhỏ/hồ, rừng/cây/động vật, mỏ và thực thể — nơi NPC sinh sống và tương tác, được tải/sinh theo nhu cầu.

Global continuity (sông, núi, địa chất) không được đứt gãy ở biên chunk.

**D. Tiêu chuẩn bắt buộc của Seed System**

| Tiêu chí | Yêu cầu |
|---|---|
| Reproducibility | Cùng seed + generator version + config → cùng thế giới |
| Domain isolation | Mỗi hệ thống có random stream độc lập |
| Coordinate stability | Sinh chunk không phụ thuộc thứ tự |
| Global continuity | Sông/núi/địa chất không đứt ở biên chunk |
| World configuration | Chọn được kích thước và đặc điểm thế giới |
| Versioning | Lưu phiên bản thuật toán sinh thế giới |
| Validation | Kiểm tra địa lý và tài nguyên sau khi sinh |
| Save/load | Khôi phục chính xác thế giới đã tạo |
| Preview | Xem bản đồ trước khi bắt đầu chơi |

**E. Versioning – seed một mình không đủ**

Seed không bảo đảm tái tạo thế giới qua các phiên bản game. Bắt buộc lưu: **world seed + generator version + world configuration + domain parameters**. Nếu thuật toán thay đổi cần chiến lược tương thích hoặc migration.

**F. Đánh giá thư viện tham khảo**

| Repository | Vai trò | Đánh giá |
|---|---|---|
| [FastNoise2](https://github.com/Auburn/FastNoise2) | Noise, địa hình, trường môi trường | Đáng thử nghiệm cho noise layer |
| [libtcod](https://github.com/libtcod/libtcod) | Heightmap, FBM, erosion, Voronoi | Chỉ tham khảo thuật toán |
| [RWorld](https://github.com/daver64/rworld) | Worldgen, climate, biome, hydrology | Audit trước — repo nhỏ, chưa rõ chất lượng |

**G. Audit prompt (bắt buộc trước khi triển khai sâu)**

Trước khi mở rộng subsystem này cần audit riêng theo prompt "SUNNYWORLD — DETERMINISTIC SEED & WORLD MAP GENERATION AUDIT", với 6 mục tiêu: seed architecture (RNG chia domain hay một stream chung), coordinate-based determinism (thứ tự chunk, song song, lặp lại), global/regional generation (lục địa, lưu vực, khí hậu theo vĩ độ, tọa độ mở rộng), world configuration & versioning (save/load, replay, migration), preview & validation (phân bố đất/liền lục địa, liên tục sông, chuyển tiếp khí hậu, plausibility tài nguyên, consistency subsurface), và repository evaluation (FastNoise2/libtcod/RWorld — dùng, tham khảo, hay nghiên cứu).

**Ràng buộc audit:** không thay thế World Generator hiện tại, không cài dependency, không sửa source, không hy sinh determinism, không làm chunk generation order-dependent, không biến tài nguyên thành object rải ngẫu nhiên, giữ nguyên tách World Truth / NPC Knowledge.

**Trạng thái đã triển khai (seed system):** domain RNG streams tường minh qua `worldgen_seed(seed, domain)` (terrain/hydrology/climate/atmosphere/geology/subsurface/resources/ecology độc lập); `WorldGenParams::origin_x/y` cho tọa độ toàn cục — elevation/moisture-base/geology noise là hàm thuần của (seed, domain, global coord), đã kiểm chứng patch ở origin khác khớp ô tương ứng của world lớn; `kGeneratorVersion` + `world_seed`/`gen_origin` lưu trên World và trong `state_hash()`; `World::save`/`load` serialize đầy đủ World Truth (entities+props, terrain, subsurface, relations, causal ledger) — roundtrip giữ nguyên `state_hash()`.

**Trạng thái đã triển khai (seed system, generator v2):**

- **Global Map tier** — mọi field là hàm giải tích thuần của (seed, domain, global cell): `elevation_at`, `is_sea`, `is_river`, `moisture_at`, `temperature_at`, `water_at`, `vegetation_at`, `wind_at`, `water_table_at`, `column_at`. Sample tọa độ bất kỳ → cùng kết quả, không cần grid. Đây là "continuous world fields" của Global Map.
- **Regional/Local tier** — `generate_region(gx0,gy0,gw,gh)` materialize bất kỳ hình chữ nhật global-cell nào trên terrain đã cấp phát, **theo thứ tự bất kỳ**: fields giống hệt (đã test 2 thứ tự ngược nhau → arrays + entity signatures giống nhau). Rivers trace descent trên elevation giải tích + upstream-inflow check — không phụ thuộc biên lưới. Moisture = base noise + sea-proximity bán kính 3 (bounded, không diffusion toàn cục).
- **Spawn dedup** — `World::claim_spawn(tag, gx, gy)` ngăn entity trùng khi region chồng lấn/regenerate; persist trong save/load và `state_hash()`.
- **Domain seeds** — `worldgen_seed(seed, domain)` cho 9 domain độc lập.
- **Versioning** — `kGeneratorVersion=2` + provenance trên World + trong hash; `load()` reject version quá mới.
- **Save/load v2** — little-endian explicit, mọi tham chiếu content (material/deposit/property/stratum) lưu bằng **tên** → save sống sót qua reorder data; roundtrip giữ nguyên `state_hash()`.
- **Preview** — `WorldGenerator::preview_ascii(world)` render terrain ra text.

**Trạng thái đã triển khai (Phase 2 hoàn tất, generator v3):**

- **Geology data-driven:** bedrock = material có `lithology` khớp lớp đá mẹ tại cell (`bedrock_class_at`), soil = material có `soil_texture` khớp khí hậu + đá mẹ — không còn tên đá/đất trong code; thêm rock/soil = sửa `materials.txt`. **Faults** (`is_fault`) là đường nứt gãy giải tích; **hydrothermal_gold_vein** chỉ hình thành trên fault (điều kiện `fault 1` trong `deposits.txt`). **Uplift+erosion** (`erosion_depth_at` = slope/2): deposit chôn nông hơn độ bóc mòn sinh ra *outcrop* (`create_deposit(..., outcrop=true)` → `exposed`), kết quả của quá trình chứ không đặt tay.
- **Hydrology:** lakes = basin (cell thấp hơn chặt cả 4 láng giềng), descent kết thúc ở sea hoặc lake; river = flow accumulation (`upstream_area` depth 3 ≥ 8 cell) + thoát ra nước đứng → kênh thưa (~5% cell) thay vì lan tràn. Groundwater (`water_table_at`) phụ thuộc độ thấm đá mẹ (sedimentary thấm sâu, igneous nông).
- **Climate:** nhiệt độ có vĩ độ (`latitude_term`, sóng tam giác chu kỳ 1024 hàng), moisture có **rain shadow** orographic (`rain_shadow_at` lấy mẫu độ cao ngược gió) + địa hình quy mô lục địa (octave 192 cell). Noise dùng floor-division → liên tục qua trục & đúng với tọa độ âm (bug cũ: gián đoạn + ngoại suy sai ở x<0).
- **Ecology data-driven:** `data/species.txt` (`SpeciesRegistry`): mỗi species là luật habitat (`condition` trên temperature/moisture/elevation/slope/near_water/vegetation/water_table/fault) + material + props + density; flora tối đa 1/cell, fauna độc lập (có `grazer`). Worldgen không chứa tên loài.
- **Config & versioning:** `World::gen_sea_level` + `seed` lưu trong save v4 và `state_hash`; `generate_region` từ chối seed/sea_level khác với thế giới đã ghi. `World::content_hash()` — digest nội dung **độc lập thứ tự** (không gồm entity id) để kiểm chứng region-order.
- **Validation (§2.6) dựa trên vật lý, tính lại từ field giải tích:** sông chảy ngược dốc (không ô thấp hơn/không nước đứng), nước đứng trên mực biển không phải basin, đất khô dưới mực biển, thực vật trên nước, giá trị ngoài 0..1000, deposit vi phạm formation condition, species ngoài habitat, flora trong sa mạc cực khô (moisture<100, không gần nước). Ngoại lệ hợp lý (gần nước) được chấp nhận.
- **Hiệu năng:** `elevation_at` có memo direct-mapped thread-local (kết quả bất biến, chỉ là cache) → sinh 128×128 ~170 ms (trước ~1.9 s).
- **Test:** 1403 checks (gồm: veins ⊂ faults, exposure == erosion, lakes là basin, region-order `content_hash`, tọa độ âm, config roundtrip, validation bắt lỗi vật lý, thêm rock + species chỉ bằng data).
- **Placer (§2.4) — generator v4:** condition field `placer_site` — deposit chỉ hình thành tại river cell có slope thấp VÀ upstream catchment (DFS ≤1024 cell ngược flow field, deterministic) chứa fault: sa khoáng là kết quả chuỗi fault-vein → xói mòn → vận chuyển → lắng đọng, không phải rải `near_water`. Surface deposit được phép trên river bed (water==2 — bãi bồi/lòng sông là mặt trầm tích lộ), vẫn cấm trên standing water (sea/lake, water==1); `validate` phân biệt hai loại nước tương tự. Test: `test_phase2_placer`.

**Gap còn lại (trung thực):** entity *ids/thứ tự vector* vẫn phụ thuộc thứ tự gọi generate_region (nội dung thì không — dùng `content_hash`); `max_trees` cap theo từng lần gọi nên chỉ order-independent khi cap không bị chạm; chưa có tectonic plate/đại dương quy mô hành tinh (relief lục địa là noise octave 192 cell + vĩ độ tuần hoàn); chưa có xói mòn/trầm tích động theo thời gian chạy (physics runtime không dùng analytic fields); `load()` vẫn yêu cầu cùng tên content; sa khoáng (placer) chọn theo điều kiện near_water/slope chứ chưa mô phỏng vận chuyển trầm tích dọc sông.

## **3. World Core – Nền tảng thế giới**

Đây là trái tim của toàn bộ engine.

World Core không biết NPC đang muốn phát minh ra vũ khí gì, không biết xã hội sẽ phát triển thành vương quốc hay bộ lạc.

Nó chỉ quản lý trạng thái thực tế của thế giới.

Ví dụ:

```
struct WorldState {
    WorldTime time;

    EntityRegistry entities;
    SpatialIndex spatialIndex;

    EnvironmentState environment;
    ResourceState resources;

    CausalEventLog eventLog;
};
```

Mỗi thực thể trong thế giới phải có trạng thái độc lập.

Ví dụ một cây:

```
struct TreeState {
    EntityId id;

    double height;
    double mass;
    double moisture;
    double health;

    MaterialId material;
    GrowthState growth;
};
```

Một tảng đá:

```
struct RockState {
    EntityId id;

    double mass;
    double density;
    double hardness;
    double brittleness;

    MaterialId material;
};
```

NPC:

```
struct AgentState {
    EntityId id;

    BodyState body;
    Inventory inventory;

    Needs needs;
    Memory memory;
    Beliefs beliefs;

    Skills skills;
    Goals goals;
};
```

Không nên tạo một class riêng cho từng loại đồ vật, động vật hoặc NPC nếu có thể biểu diễn chúng bằng Entity + Components.

**Yêu cầu kiến trúc:** thêm một loại thực thể mới không được yêu cầu viết lại World Core.

## **4. Entity & Ontology – Hệ thống định nghĩa thực thể**

Đây là module giúp engine hiểu những thứ tồn tại trong thế giới.

Tôi đề xuất sử dụng mô hình Entity – Component – Property – Relationship.

Ví dụ:

**Entity: Một tảng đá**

**Identity**

ID: rock_001 · Type: Physical Entity

**Properties**

- Mass
- Density
- Hardness
- Brittleness
- Thermal Conductivity

**Components**

- Material Composition
- Physical Structure
- Spatial Position

**Relationships**

- Located on terrain
- Can collide with other objects
- Can be fractured
- Can interact with other materials

Một điểm rất quan trọng: không nên chỉ dùng một danh sách thuộc tính dạng `string -> float`.

Ví dụ `hardness = 0.8` không có ý nghĩa khoa học nếu không xác định thang đo, đơn vị và mô hình đo.

Hệ thống Property cần hỗ trợ:

- Kiểu dữ liệu.
- Đơn vị đo.
- Phạm vi hợp lệ.
- Điều kiện áp dụng.
- Quan hệ phụ thuộc.
- Độ bất định khi đo đạc.
- Nguồn gốc của giá trị.

Ví dụ:

```
struct PropertyDefinition {
    PropertyId id;
    ValueType type;
    UnitId unit;

    ValidationRange range;
    PropertyDomain domain;
};
```

Nhờ đó, một vật liệu mới có thể được thêm vào bằng dữ liệu, miễn là các đặc tính của nó thuộc những loại mà engine hiểu được.

## **5. Natural Law System – Hệ thống quy luật tự nhiên**

Đây là phần quyết định thế giới có thực sự vận hành theo nhân quả hay không.

Tôi không khuyên xây dựng một Universal Rule Engine chỉ dựa trên những câu lệnh `if/else`.

Thay vào đó, nên có một hệ thống quy luật nhiều cấp.

**Natural Law Registry**

**Physics**

Lực, chuyển động, va chạm, nhiệt, năng lượng, áp suất, vật liệu.

**Chemistry**

Thành phần, phản ứng, cháy, oxy hóa, phân hủy, biến đổi vật chất.

**Biology**

Trao đổi chất, sinh trưởng, di truyền, sinh sản, đột biến, bệnh tật.

**Ecology**

Chuỗi thức ăn, cạnh tranh, môi trường sống, quần thể và hệ sinh thái.

Mỗi lĩnh vực có một bộ giải riêng nhưng cùng tuân thủ một giao diện:

```
class INaturalLawModel {
public:
    virtual ~INaturalLawModel() = default;

    virtual bool supports(
        const InteractionContext& context
    ) const = 0;

    virtual PredictionResult predict(
        const WorldSnapshot& world,
        const InteractionContext& context
    ) const = 0;

    virtual void apply(
        WorldState& world,
        const PredictionResult& result
    ) const = 0;
};
```

Ví dụ:

- Physics Model xử lý va chạm.
- Chemistry Model xử lý phản ứng hóa học.
- Biology Model xử lý sinh sản và di truyền.
- Ecology Model xử lý biến động quần thể.

### **Nguyên tắc không được phá vỡ**

Nếu NPC không biết lửa có thể đốt gỗ, lửa vẫn phải đốt gỗ.

Nếu NPC tin rằng đá mềm hơn gỗ, niềm tin đó không được thay đổi tính chất thực tế của đá.

Nếu NPC thất bại trong một thí nghiệm, thế giới vẫn vận hành theo các quy luật khách quan.

Đây là sự phân tách giữa:

- World Truth.
- Agent Belief.
- Experimental Evidence.

Ba thứ này tuyệt đối không được nhập làm một.

## **6. Interaction System – Hệ thống tương tác phổ quát**

Đây là nơi các thực thể có thể tác động lên nhau.

Tôi đề xuất không tạo danh sách cố định kiểu:

```
enum Interaction {
    CUT_WOOD,
    BURN_WOOD,
    SMELT_METAL,
    BREED_ANIMALS
};
```

Vì càng thêm nhiều tương tác, engine càng phụ thuộc vào code cứng.

Thay vào đó, dùng mô hình:

\\[ Interaction = Entities + Conditions + Laws \\]

Ví dụ một NPC tác động một hòn đá lên cành cây:

- NPC cung cấp lực.
- Hòn đá có độ cứng.
- Cành cây có độ bền.
- Hai vật thể có vị trí và hướng.
- Physics Model tính toán va chạm.
- Material Model đánh giá biến dạng hoặc gãy.

Kết quả không được định trước.

Có thể là:

- Cành cây không thay đổi.
- Cành cây bị nứt.
- Cành cây gãy.
- Đá bị vỡ.
- Cả hai cùng bị hư hại.

Engine chỉ đưa ra kết quả theo điều kiện thực tế.

Một interaction phải có cấu trúc như sau:

```
struct InteractionContext {
    EntityId actor;
    std::vector<EntityId> targets;

    ActionParameters parameters;
    WorldSnapshot snapshot;
};

struct InteractionResult {
    bool completed;

    std::vector<StateChange> changes;
    std::vector<CausalEvent> events;

    ResourceCost cost;
};
```

Lưu ý: `ActionParameters` mô tả hành động vật lý mà NPC thực hiện, không phải tên sản phẩm mà NPC muốn engine tạo ra.

### **6.1. Affordance & Engine-Level Constraints**

Engine không được có quy tắc riêng kiểu `if (target == TREE) allow(CHOP)`. Hành động phải phụ thuộc vào đặc tính, cấu trúc và điều kiện của vật thể:

- Một thân cây có thể bị tác động bằng lực, bị cắt nếu công cụ và điều kiện phù hợp, bị đốt nếu đủ điều kiện cháy.
- **Affordance là khả năng thực tế của thế giới; Knowledge là những gì NPC biết về khả năng ấy.** NPC không nhất thiết biết tất cả affordance của một vật.

Constraint & Conservation được kiểm tra ở **cấp engine**, không chỉ là quy tắc hành vi NPC:

- Không tạo gỗ từ hư không.
- Không chặt cây từ khoảng cách không thể tiếp cận.
- Không xây nhà nếu thiếu vật liệu cần thiết.
- Không vận chuyển vật nặng vượt khả năng.
- Không tạo kim loại nếu quá trình vật lý/hóa học không cho phép.

Constraint & Possibility Engine xác định điều gì có thể và không thể xảy ra — để NPC không thực hiện được hành động bất khả thi ngay cả khi nó tin là có thể.

## **7. Causal Engine – Bộ máy nhân quả**

Module cực kỳ quan trọng

Tôi muốn Sunnyworld có một Causal Engine độc lập.

Nhiệm vụ của nó là trả lời:

1. Điều gì đã xảy ra?
2. Nguyên nhân trực tiếp là gì?
3. Điều kiện nào khiến sự kiện xảy ra?
4. Những quy luật nào tham gia?
5. Kết quả làm thay đổi trạng thái nào?
6. Nếu thiếu một điều kiện thì kết quả có thay đổi không?

Ví dụ:

```
NPC dùng đá đập vào cành cây
        |
        v
Tạo lực va chạm
        |
        v
Lực tác động vượt ngưỡng chịu lực
        |
        v
Cành cây bị nứt
        |
        v
Cấu trúc mất độ bền
        |
        v
Cành cây gãy
```

Đây là causal chain.

Một sự kiện phải ghi lại được nguồn gốc:

```
struct CausalEvent {
    EventId id;

    WorldTime timestamp;

    std::vector<EntityId> participants;
    std::vector<EventId> parentEvents;

    std::vector<LawId> appliedLaws;

    std::vector<StateChange> changes;

    EventEvidence evidence;
};
```

`parentEvents` cho phép engine truy ngược các sự kiện dẫn đến kết quả.

Ví dụ khi một NPC phát minh được vũ khí, engine có thể truy lại:

- Vật liệu lấy từ đâu.
- Ai đã tìm ra phương pháp chế tạo.
- Thử nghiệm nào thành công.
- Quy luật nào giải thích khả năng hoạt động.
- Ai đã truyền bá phương pháp đó.

Điều này cực kỳ hữu ích khi bạn muốn kiểm tra một nền văn minh đã tiến hóa như thế nào sau hàng nghìn năm.

## **8. Composition Engine – Hệ thống kết hợp và tạo ra thứ mới**

Một trong những module quan trọng nhất của mục tiêu C

Đây là module giải quyết trực tiếp ý tưởng:

\\[ A+B \rightarrow C \\]

Nhưng tôi muốn thiết kế sâu hơn một chút.

Engine không nên hiểu rằng cứ kết hợp A và B là sinh ra C. Nó phải hiểu những điều kiện khiến A và B có thể tạo ra một cấu trúc mới.

Tôi đề xuất chia Composition Engine thành 4 hệ thống.

### **8.1. Material Composition – Kết hợp vật liệu**

Ví dụ:

[Flint](https://images.openai.com/static-rsc-4/FQxtY90ZZHLhRnHbarbg57LrFY4uRaZQj_-8n8xP1mGlp-aBahweNnfIL3ZS4OoK_-HZEybCNBE2caABuFKodSyhyNjDTMr9NqOZbvZ89u0NMGDpqQrc3FOsmvdk_7fmvFNyorvRk4io5p1rM888VXQ9ciqSbUlcQDOtMmo9I3WS64J5o7Pcgom690BUl9NR?purpose=inline)

[The Stone Age Timeline Explained](https://images.openai.com/static-rsc-4/ODoCJ76viFvCddzEFbbVXFeOte-GY_a0vykaQ2aHvh06DXbV3reRl7MGpuhrNoijLPov0rbQnrHtpOAp08r1Kz92hrQ3rzaI-jB3OuNy4Rk5tPm7se-vIEM0vbkkKVrGq2FLQiJ3PwY7I2bmApC5HArXvDOYFfC5pu4LNgHyH70?purpose=inline)

[Forscherinnen zerlegen Vögel wie Neandertaler früher | WEB.DE](https://images.openai.com/static-rsc-4/KiL6gE0nsfl4Q-0j25QTTSBC1IZuw6nWfep10_Y9Y4RVXeRBe3txcVZsyorsav5GhSe2BY-A0ejkTsHzQp--_qhHo2cOh37CzpSUzS86Abtl-GMeBUZbPCyIq2kaP6oobUhPiZbKgjmppsye0gvElNUAumGyYuc70h5eohDZ4rk?purpose=inline)

9

Một NPC có:

- Đá sắc.
- Cành cây.
- Dây leo.

Composition Engine đánh giá:

| Điều kiện               | Kết quả đánh giá            |
| ----------------------- | --------------------------- |
| Đá có thể gắn vào gỗ?   | Có thể nếu cấu trúc phù hợp |
| Gỗ có chịu được lực?    | Kiểm tra độ bền             |
| Dây leo có giữ được đá? | Kiểm tra độ bền liên kết    |
| Cấu trúc có ổn định?    | Kiểm tra cơ học             |
| Sản phẩm có hoạt động?  | Thử nghiệm thực tế          |

Nếu đáp ứng điều kiện, engine có thể tạo ra một entity mới với cấu trúc:

```
Composite Object
├── Wooden Shaft
├── Stone Head
└── Vine Binding
```

Nó không phải một sản phẩm có sẵn trong database.

Nó là một cấu trúc mới được tạo từ các thực thể có sẵn.

### **8.2. Property Inheritance – Kế thừa đặc tính**

Vật thể mới phải có đặc tính phát sinh từ các thành phần.

Ví dụ:

| Đặc tính            | Nguồn            |
| ------------------- | ---------------- |
| Độ sắc              | Đầu đá           |
| Độ cứng             | Đầu đá           |
| Độ linh hoạt        | Thân gỗ          |
| Độ bền kết nối      | Dây buộc         |
| Khả năng truyền lực | Toàn bộ cấu trúc |

Tuy nhiên, không phải mọi đặc tính đều có thể tính bằng trung bình cộng.

Ví dụ:

- Độ bền của cấu trúc phụ thuộc vào điểm yếu nhất.
- Khả năng chịu lực phụ thuộc vào hình dạng và cách liên kết.
- Khối lượng tổng có thể tính từ tổng khối lượng các thành phần.
- Độ sắc có thể bị ảnh hưởng bởi hình học của cạnh.

Do đó, mỗi nhóm đặc tính cần một mô hình tính toán thích hợp.

### **8.3. Emergent Properties – Đặc tính phát sinh**

Đây là phần thú vị nhất.

Khi kết hợp các thành phần, cấu trúc mới có thể có những khả năng mà từng thành phần riêng lẻ không có.

Ví dụ:

- Đá riêng lẻ có thể sắc.
- Gỗ riêng lẻ có thể dài và chịu lực.
- Khi gắn chúng lại, cấu trúc có thể đâm từ khoảng cách xa hơn.

Khả năng mới này không tồn tại trong từng vật liệu riêng biệt.

Nó xuất hiện từ cách chúng được tổ chức.

Engine cần đánh giá cấu trúc tổng thể chứ không chỉ cộng thuộc tính.

### **8.4. Biological Composition – Kết hợp sinh học**

Đối với sinh vật, cần một hệ thống riêng.

Không thể lấy hai sinh vật rồi cộng trực tiếp sức mạnh, tốc độ và trí thông minh.

Tôi đề xuất:

```
Parent A Genome
       |
       v
Recombination
       |
       v
Parent B Genome
       |
       v
Mutation
       |
       v
Gene Expression
       |
       v
Offspring Phenotype
       |
       v
Survival & Natural Selection
```

Mô hình sinh học phải có:

- Bộ gene hoặc mô hình di truyền tương đương.
- Gene và biến thể gene.
- Tương tác giữa gene.
- Tính trạng trội và lặn.
- Đột biến.
- Tương thích sinh sản.
- Phát triển cơ thể.
- Khả năng sinh tồn và sinh sản.

Ví dụ, một thế hệ con có thể nhận đặc tính tốc độ từ bố và sức mạnh từ mẹ, nhưng kết quả cuối cùng còn phụ thuộc vào biểu hiện gene, tương tác gene và sự phát triển.

**Kết luận:** Composition Engine nên có một lõi tổng quát về cấu trúc, thành phần và thuộc tính; còn các lĩnh vực như vật liệu, hóa học, sinh học cần những bộ giải chuyên biệt.

### **8.5. Parametric Physical Geometry – hình học vật lý nhẹ (core solver layer)**

Quyết định kiến trúc: **không chấp nhận reach-proxy cho tương tác vật lý**. Emergent tool function và material interaction phụ thuộc trực tiếp vào tỉ lệ, phân bố khối lượng và điểm gắn kết cấu — không phải proximity. Không dùng mesh 3D/CAD nặng; thay vào đó là **biểu diễn primitive tham số hóa nhẹ** ngay trong core solver, để mọi law đọc cùng một mô hình hình học.

#### **8.5.1. Primitive representation**

Mỗi thực thể/cấu trúc mang một tập **oriented bounding primitives** — không phải một bounding box duy nhất:

| Primitive | Dùng cho | Tham số bắt buộc |
|---|---|---|
| `rod` / `capsule` | thân giáo, cán, xương, dây | length, radius/width, orientation |
| `plate` / `box` | lưỡi đá, mảnh vỡ, tấm | length, width, thickness, orientation |
| `sphere` / `blob` | cục, hạt, cơ quan | radius |
| `socket` (điểm gắn) | mọi primitive có thể mang socket | offset trong không gian primitive, loại chấp nhận |

Mỗi composite lưu danh sách primitives + sockets của từng part, định vị tương đối theo quan hệ `part_of` — **attachment offset là dữ liệu**, không phải code.

#### **8.5.2. Derived physical quantities (solver tính, không lưu tay)**

Solver suy ra từ primitives + mass, tất cả fixed-point integer:

- **Khối tâm (CoM)** và **moment of inertia xấp xỉ** theo trục quay — cần cho đòn bẩy.
- **Angular momentum / impact leverage proxy**: `L ∝ m·r²` với r = khoảng cách từ điểm cầm đến CoM theo hướng vung — giáo dài + đầu nặng ở xa cán tự nhiên cho đòn mạnh hơn mà không cần tag "hammer".
- **Thể tích chiếm chỗ / displacement** và **mật độ tổng** — bè rộng + vật liệu nhẹ tự nhiên nổi; không cần tag "raft".
- **Diện tích bề mặt / tiếp xúc** — dẫn nhiệt, ma sát, đón gió.
- **Balance / stability proxy**: hình chiếu CoM lên mặt phẳng tựa so với đa giác chống đỡ — cấu trúc lệch trọng tâm tự đổ.
- **Reach hiệu dụng**: bán kính với tới = f(chiều dài cấu trúc, điểm cầm) — thay thế hoàn toàn reach-proxy hiện tại.

#### **8.5.3. Structural fit & attachment rules**

Kết nối là **physics-generic**, không phải recipe: mỗi cặp socket đánh giá theo:

- **Loại tương thích**: binding (quấn), wedging (nêm), piercing (đâm xuyên), balancing (đặt lên) — khai báo trên socket/part data.
- **Ràng buộc hình học**: kích thước socket vs kích thước part, hướng/orientation, khoảng hở.
- **Ràng buộc vật liệu**: tensile strength, friction, hardness của phần liên kết so với tải trọng tĩnh/dynamic.
- **Kết quả**: `FitResult { feasible, joint_strength, failure_mode }` — joint yếu trở thành điểm gãy tiềm năng của composite (weakest link lan xuống tận socket).

Solver từ chối hoặc chấp nhận assemble dựa trên FitResult — `too fragile` hiện tại chỉ là một trường hợp suy biến của luật này.

#### **8.5.4. Tương tác downstream**

- **Physics/impact**: damage = f(E, hardness, brittleness, **I·ω² proxy từ geometry**, điểm tiếp xúc) — hai cấu trúc cùng material nhưng khác tỉ lệ cho kết quả khác nhau.
- **Buoyancy/fluid** (khi có hydrology runtime): displacement vs weight từ primitives.
- **Thermal**: diện tích bề mặt từ primitives thay vì chỉ thermal_mass.
- **2.5D render alignment (§20.3)**: cùng primitives + socket offsets lái sprite layer transforms — renderer lắp composite đúng hình học simulation định nghĩa, không tự suy diễn.

#### **8.5.5. Ràng buộc**

- Pure integer/fixed-point; mọi derived quantity deterministic và vào `state_hash()` (primitives là World Truth, không phải render state).
- Materials khai báo **primitive profile mặc định** trong `materials.txt` (rod/plate/sphere + dims); entity có thể override — data-driven, không hardcode.
- Composite geometry = union primitives của parts + joint transforms từ socket fit — solver lưu cấu trúc, không lưu mesh.
- Nguyên tắc giữ nguyên: code định nghĩa cách hình học suy ra vật lý; data định nghĩa hình dạng từng vật.

# **9. Natural Resource & Extraction Engine – Tài nguyên tồn tại trước khi được biết đến**

Đây là tầng trung gian bắt buộc giữa World Simulation và Material System. Nếu thiếu tầng này, engine sẽ ngầm giả định vật liệu "có sẵn để dùng", và toàn bộ chuỗi khám phá tài nguyên – khai thác – chế biến sẽ không thể nảy sinh một cách tự nhiên.

## **9.1. Năm khái niệm phải được phân biệt hoàn toàn**

Ví dụ quặng đồng:

| Khái niệm | Ý nghĩa | Ví dụ |
|---|---|---|
| Natural Substance | Chất tồn tại trong tự nhiên | Đồng nguyên tố, khoáng vật chứa đồng |
| Natural Deposit | Nơi chất đó tồn tại | Mỏ quặng đồng |
| Resource Occurrence | Dạng tài nguyên trong môi trường | Quặng đồng nằm dưới lòng đất |
| Extracted Resource | Tài nguyên đã khai thác | Đá quặng được đào lên |
| Processed Material | Vật liệu đã qua chế biến | Đồng kim loại |

Một mỏ đồng không phải một thỏi đồng. Một khối quặng đồng không đồng nghĩa với đồng kim loại có thể dùng để chế tạo công cụ. NPC phải tự tìm hiểu cách khai thác và xử lý nó.

## **9.2. Pipeline tài nguyên và vòng phản hồi**

```
WORLD GENERATION
Địa chất · khí hậu · địa hình · hệ sinh thái
        |
        v
NATURAL OCCURRENCE
Khoáng sản · cây cối · động vật · nước · đất
        |
        v
RESOURCE DISCOVERY      (quan sát · thăm dò · nhận diện · khảo sát)
        |
        v
EXTRACTION ENGINE       (đào · chặt · săn · thu thập · khai thác)
        |
        v
RAW RESOURCE PROCESSING (đập · nghiền · sấy · tách · nung · tinh luyện)
        |
        v
MATERIAL SYSTEM         (vật liệu đã tạo ra, sẵn sàng tương tác)
        |
        v
COMPOSITION & APPLICATION (kết hợp · chế tạo · xây dựng · phát minh)
```

Pipeline này có vòng phản hồi: khai thác thay đổi địa chất và hệ sinh thái; chế biến tạo chất thải và tiêu hao nhiên liệu; công cụ mới lại thay đổi khả năng khai thác.

## **9.3. Resource Existence khác Resource Knowledge**

Tài nguyên phải tồn tại trong thế giới **trước khi** NPC biết đến chúng.

Ví dụ một khu vực có quặng đồng dưới đất, quặng sắt trong tầng đá, than sâu dưới lòng đất, đá ở sườn núi, gỗ trong rừng, động vật trong hệ sinh thái. Engine phải biết những tài nguyên này tồn tại — nhưng NPC không được tự động biết vị trí hay thành phần của chúng.

```
WORLD TRUTH                          AGENT BELIEF
Location: Mountain Region A          NPC chỉ nhìn thấy:
  Underground:                         núi, cây cối, địa hình
    - Copper-bearing ore              Chưa biết bên dưới có quặng đồng
    - Iron-bearing ore
    - Rock formations
  Surface:
    - Forest, Wildlife, Water
```

Khi NPC quan sát đá, đào đất hoặc phát hiện dấu hiệu địa chất, nó mới dần hình thành giả thuyết. **Resource Existence và Resource Knowledge là hai thứ khác nhau và không bao giờ được nhập làm một.**

## **9.4. Khai thác cũng phải được khám phá**

Không được lập trình sẵn `mine(copper_ore)`. Engine chỉ cung cấp các thao tác vật lý tổng quát:

- Tác dụng lực, đào đất, đập đá, tách vật thể.
- Cắt, kéo, nâng, đốt, nung, nghiền.

Các thao tác này chịu sự chi phối của vật lý và đặc tính vật liệu. Ví dụ lấy đá từ sườn núi:

1. NPC quan sát bề mặt đá, nhận thấy một số mảnh có thể tách ra.
2. Dùng tay thử tác động — thất bại vì lực không đủ.
3. Thử dùng hòn đá khác để tạo lực va đập.
4. Một mảnh đá vỡ ra — NPC nhận được tài nguyên mới.
5. Sau đó có thể khám phá cách chế tạo công cụ giúp khai thác hiệu quả hơn.

Không cần công nghệ `Stone Mining` được định nghĩa trước. Engine chỉ cần mô phỏng đúng tương tác.

## **9.5. Processing Engine – module riêng, không gộp vào Composition**

Ví dụ quặng đồng:

```
Copper-bearing Ore → Extraction → Raw Ore → Crushing/Sorting
  → Ore Preparation → Heating/Smelting → Metal Product
  → Casting/Shaping → Copper Tool
```

Tất cả các bước phải có điều kiện thực tế:

- Nhiệt độ không đủ thì không đạt được kết quả mong muốn.
- Không có nhiên liệu thì không duy trì được quá trình nung.
- Quặng có thành phần không phù hợp thì không tạo được sản phẩm.
- Công cụ chế biến không phù hợp có thể làm thất thoát tài nguyên.

Engine không được biến quặng thành đồng kim loại chỉ vì NPC muốn chế tạo công cụ đồng.

## **9.6. Tài nguyên sinh học và khoáng sản**

Không chỉ khoáng sản mới cần khai thác:

| Tài nguyên tự nhiên | Quá trình | Kết quả |
|---|---|---|
| Cây sống | Chặt | Gỗ |
| Cây có nhựa | Thu nhựa | Nhựa thực vật |
| Quặng đồng / sắt | Đào, tách | Quặng thô |
| Động vật | Săn bắt | Thịt, da, xương |
| Cây trồng hoang dại | Thu hoạch | Hạt, quả |
| Đất sét | Đào | Đất sét thô |
| Sông | Thu nước | Nước |
| Đá | Đập, tách | Mảnh đá |

Kết quả thu được phải phụ thuộc điều kiện khai thác: chặt cây không phải lúc nào cũng tạo gỗ tốt; săn động vật không phải lúc nào cũng thu được nguyên vẹn mọi bộ phận.

## **9.7. Mô hình dữ liệu**

Không dùng `Material` làm thực thể đại diện cho toàn bộ tài nguyên tự nhiên. Thay vào đó:

- **Natural Resource** – tài nguyên tồn tại trong môi trường.
- **Resource Deposit** – vị trí, trữ lượng, thành phần, phân bố, độ sâu, khả năng tiếp cận.
- **Extracted Resource** – tài nguyên đã lấy ra, vẫn có thể chứa nhiều thành phần.
- **Processed Material** – vật liệu sau các quy trình chế biến.
- **Composite Object** – cấu trúc kết hợp từ vật liệu và thành phần.

```
resource_occurrence:
  id: copper_deposit
  geological_context:
    host_rock: rock_formation_type_A
    depth_range: [5, 30]
  composition:
    - substance: copper_bearing_mineral
      proportion: variable
    - substance: host_rock
      proportion: variable
  physical_state:
    solid: true
    accessibility: underground
  properties:
    hardness: measured_by_model
    density: measured_by_model
```

Đây là dữ liệu World Truth, không phải dữ liệu NPC được phép đọc trực tiếp.

## **9.8. Bài kiểm chứng kiến trúc**

Tạo một thế giới có mỏ quặng đồng, nhưng không có NPC nào biết đến đồng. Điều kiện ban đầu:

- Mỏ quặng đã tồn tại trong World Truth.
- NPC không có kiến thức về khai thác.
- Không có công nghệ khai khoáng, luyện kim, hay công thức chế tạo công cụ đồng.

Sau nhiều thế hệ, NPC có thể: phát hiện đá khác thường → tìm ra khu vực khoáng sản → khám phá cách khai thác → tách quặng → thử phương pháp gia nhiệt → tạo kim loại → tạo hình → ứng dụng vào công cụ và vũ khí.

Không nhất thiết mọi nền văn minh đều làm được — điều đó phụ thuộc tài nguyên, điều kiện, kiến thức và khả năng của họ.

**Tiêu chuẩn thành công:** nếu thay đổi vị trí mỏ, thành phần quặng hoặc đặc tính địa chất, con đường khám phá và kết quả của NPC phải có khả năng thay đổi theo.

## **9.9. Nguyên tắc chốt**

Không chỉ công nghệ cần được khám phá — **bản thân tài nguyên, vị trí tài nguyên, cách khai thác và cách chế biến cũng phải được khám phá.**

Chuỗi kiến trúc tổng quát:

```
WORLD TRUTH → OBSERVATION → DISCOVERY → EXTRACTION
  → PROCESSING → COMPOSITION → APPLICATION → EVOLUTION
```

Vòng lặp tiếp tục khi kết quả tạo ra nhu cầu, tài nguyên và khả năng mới.

Mục tiêu: thêm một mỏ khoáng sản hoàn toàn mới vào thế giới **không cần viết một dòng code riêng** để NPC biết khai thác nó. Engine chỉ cần hiểu đặc tính vật lý, hóa học và địa chất của nó; phần còn lại là quá trình khám phá của thế giới.

## **9.10. Resource Perception & Discovery – NPC biết tài nguyên qua dấu hiệu, không qua World Truth**

Nguyên tắc cốt lõi:

> Engine biết tài nguyên ở đâu. NPC chỉ biết những dấu hiệu mà chúng có thể quan sát được. Từ những dấu hiệu đó, NPC phải tự tìm hiểu, xác định và khám phá cách khai thác.

**Hai lớp thông tin tuyệt đối không được đồng bộ tự động:**

- **World Truth:** mạch quặng ở độ sâu 8m, thành phần khoáng vật đồng, trữ lượng, phân bố — NPC không được truy cập trực tiếp.
- **NPC Knowledge:** chỉ những gì quan sát được — đá màu khác thường, viên đá lạ, dấu hiệu địa chất. NPC chưa biết đó là quặng đồng, chưa biết khai thác được kim loại.

### **9.10.1. Năm cơ chế NPC nhận biết tài nguyên**

1. **Observation** – nhìn thấy trong phạm vi cảm nhận: cây, đá, sông, động vật, dấu vết, màu sắc, địa hình.
2. **Direct Interaction** – chạm, nhặt, nếm, ngửi, đập, đào, thử tác động; mỗi hành động tạo bằng chứng mới.
3. **Indirect Evidence** – dấu chân, phân, cây bị ăn, đá lộ thiên, đất xói mòn, nước mang khoáng chất.
4. **Purposeful Survey** – chủ động tìm nước, thức ăn, đá, gỗ, khu vực trú ẩn; ghi nhớ phát hiện.
5. **Knowledge Transmission** – học từ người khác qua giao tiếp, quan sát, dạy học, ghi chép.

### **9.10.2. Observable Evidence là dữ liệu, không phải truth**

Mỗi tài nguyên tự nhiên có hai nhóm thuộc tính:

| World Truth | Observable Evidence |
|---|---|
| Vị trí thật | Có nhìn thấy được không? |
| Thành phần | Màu sắc, hình dạng |
| Trữ lượng | Dấu hiệu về số lượng |
| Độ sâu | Có dấu hiệu lộ thiên không? |
| Độ cứng | Có thử được bằng thao tác vật lý không? |
| Khả năng khai thác | Không được tiết lộ trực tiếp |

Quặng sâu 8m không tự xuất hiện trong nhận thức NPC. Khi mưa xói mòn đất để lộ mạch khoáng, NPC đi ngang mới có thể nhìn thấy đá khác thường. **World Simulation tạo hiện tượng → Perception chuyển thành dữ liệu cảm nhận → Discovery giúp suy luận.**

### **9.10.3. Pipeline nhận thức tài nguyên**

```
WORLD TRUTH (tài nguyên, địa chất, sinh thái)
   → hiện tượng có thể quan sát
PERCEPTION ENGINE (vision, touch, environmental evidence)
   → dữ liệu cảm nhận có giới hạn
BELIEF & KNOWLEDGE (ghi nhớ, phân loại, đánh giá độ tin cậy)
   → giả thuyết về tài nguyên và khả năng khai thác
EXPERIMENTATION & EXTRACTION (tiêu hao thời gian, công cụ, năng lượng)
   → kết quả thực nghiệm
VERIFIED KNOWLEDGE → truyền bá cho cộng đồng
```

### **9.10.4. Nguyên tắc bổ sung**

**NPC không nhất thiết phải biết tên tài nguyên mới có thể khai thác nó.** NPC chưa biết khái niệm đồng vẫn có thể nhặt đá màu xanh, đập vỡ nó, nung trong lửa và quan sát biến đổi vật chất — phát hiện công dụng trước khi hiểu bản chất. Đây là điều giúp thế giới tiến hóa tự nhiên thay vì dựa trên cây công nghệ.

**Không lập trình `available_resources = [wood, copper, stone]`.** NPC cần khả năng nhận biết môi trường, phát hiện dấu hiệu, hình thành giả thuyết và thử nghiệm — nhờ đó engine có thể thêm hàng nghìn loại tài nguyên mà không viết riêng logic khám phá cho từng loại.

### **9.10.5. Ánh xạ vào roadmap (tránh làm sai thứ tự)**

| Thành phần | Phase |
|---|---|
| Deposit/Occurrence + observable evidence properties (dữ liệu World Truth) | Phase 1 |
| World Generation: deposits sinh ra từ quy tắc địa chất, không rải ngẫu nhiên | Phase 2 |
| Extraction & Processing law models | Phase 3 |
| Perception Engine: lọc World Truth → Observation theo phạm vi/điều kiện | Phase 5 |
| Belief/Memory ghi nhận observation, không ghi truth | Phase 5 |
| Resource hypothesis, survey, experimentation → verified knowledge | Phase 6 |
| Truyền bá kiến thức tài nguyên trong cộng đồng | Phase 8 |

Thứ tự này bắt buộc: worldgen (Phase 2) cần deposit schema của Phase 1 để đặt occurrence; không thể có perception trước khi có occurrence/extraction trong World Truth; và không thể có discovery trước khi có cả hai.

# **10. Agent Intelligence – Hệ thống trí tuệ NPC**

NPC không thể chỉ là một danh sách hành động được viết trước.

Nhưng cũng không cần phải sử dụng LLM hoặc Machine Learning để mỗi NPC trở nên thông minh.

Tôi đề xuất một kiến trúc Cognitive Agent thuần thuật toán.

## **Cognitive Architecture**

**Perception – Nhận thức**

Nhìn, nghe, chạm, ngửi, cảm nhận nhiệt độ, nhận biết sự kiện.

**Memory – Bộ nhớ**

Lưu trữ trải nghiệm, sự kiện, địa điểm, hành động và kết quả.

**Belief System – Niềm tin**

NPC hình thành nhận thức riêng về cách thế giới hoạt động.

**Reasoning – Suy luận**

Tìm nguyên nhân, đưa ra giả thuyết và đánh giá khả năng.

**Planning – Lập kế hoạch**

Chọn hành động dựa trên nhu cầu, mục tiêu và nguồn lực.

**Action – Hành động**

Tương tác với thế giới thật thông qua Natural Law và Causal Engine.

### **10.1. NPC phải có giới hạn nhận thức**

Đây là điều rất quan trọng.

Ví dụ:

NPC A nhìn thấy cây bị cháy sau một trận cháy rừng.

NPC A không tự động biết:

- Cấu trúc hóa học của gỗ.
- Phản ứng oxy hóa.
- Nhiệt độ bắt cháy.
- Cách tạo ra lửa.

Nó chỉ có thể ghi nhận:

```
Observation:
A tree was burning.

Context:
Forest fire.

Consequences:
The tree became damaged.
Nearby animals escaped.

Hypothesis:
Fire may cause damage to wood.
```

Đó là lượng thông tin mà NPC có thể tiếp cận.

NPC khác không nhìn thấy sự kiện này thì không được tự động biết nó.

### **10.2. Hệ thống niềm tin**

Mỗi NPC có thể có các mức độ tin tưởng khác nhau:

| Trạng thái   | Ý nghĩa                  |
| ------------ | ------------------------ |
| Unknown      | Chưa biết                |
| Observed     | Đã quan sát              |
| Hypothesized | Đã hình thành giả thuyết |
| Tested       | Đã thử nghiệm            |
| Supported    | Có bằng chứng hỗ trợ     |
| Verified     | Đã xác minh              |
| Disputed     | Có bằng chứng mâu thuẫn  |

Không được biến một giả thuyết thành sự thật chỉ vì NPC tin vào nó.

### **10.3. Hệ thống suy luận**

Tôi đề xuất kết hợp:

- Forward chaining: suy luận từ những gì đã biết.
- Backward chaining: suy ngược từ mục tiêu.
- Causal reasoning: suy luận nguyên nhân.
- Search algorithms: tìm chuỗi hành động khả thi.
- Analogical reasoning: áp dụng kinh nghiệm cũ vào tình huống mới.

Ví dụ NPC cần tự vệ.

Nó biết:

- Thú dữ có thể gây thương tích.
- Đá có thể làm động vật bị thương.
- Gỗ có thể được cầm và sử dụng.

Nó có thể tìm một chuỗi hành động khả thi:

```
Collect Stone
    ↓
Collect Branch
    ↓
Test Stone Against Branch
    ↓
Attempt Attachment
    ↓
Evaluate Stability
    ↓
Test Against Target
```

Không có bước nào bắt buộc phải mang tên `CraftSpear`.

Engine chỉ tìm những thao tác khả thi từ những gì NPC đã biết và có thể thực hiện.

### **10.4. Goal Decomposition & Planning – engine phải suy ra hành động khả thi**

Nếu chỉ viết `if (npc.needs_wood) { npc.chop_tree(); }` thì vẫn là hardcode, dù có hàng nghìn loại tài nguyên.

Thay vào đó, engine cần **Goal Decomposition & Planning System**:

```
NPC has a goal
      ↓
What conditions must be satisfied?
      ↓
What resources or states could satisfy them?
      ↓
What does the NPC know about those resources?
      ↓
What actions can change the current world state?
      ↓
Which action sequence is feasible?
      ↓
Can the NPC perform it with current abilities?
      ↓
Execute actions → World changes theo natural laws
      ↓
Evaluate result → Learn, adapt, or try another approach
```

Planner **không được biết trước mọi con đường đến mọi mục tiêu**. Nó tìm chuỗi hành động khả thi từ: trạng thái hiện tại, mục tiêu, kiến thức NPC sở hữu, hành động NPC thực hiện được, điều kiện vật lý của thế giới, nguồn lực và giới hạn của NPC.

Đây là khác biệt giữa NPC thực hiện kịch bản và NPC có khả năng giải quyết vấn đề. Nếu bốn bộ máy — nhân quả, tương tác, nhận thức, lập kế hoạch — đủ tốt, engine có nền tảng thêm nội dung mà không phải viết lại logic game.

### **10.5. Uncertainty & Probability**

Engine phải xử lý hai loại bất định:

- **World stochasticity:** sự kiện ngẫu nhiên (thời tiết, đột biến, gặp gỡ) đi qua deterministic RNG streams — tái hiện được khi replay nhưng không đoán trước được bởi agent.
- **Agent uncertainty:** NPC chỉ có thông tin không đầy đủ — có thể đoán sai, thử sai và gặp may. Belief có mức độ tin cậy; giả thuyết sai vẫn có thể được thử nghiệm và thất bại.

### **10.6. Feedback & Adaptation**

NPC không được lặp mãi một phương pháp đã thất bại. Kết quả thử nghiệm phải điều chỉnh niềm tin và hành vi: thất bại lặp lại làm giảm độ tin cậy của giả thuyết; thành công tăng cường nó. Đây là cầu nối giữa Experimentation (mục 11) và Belief states (mục 10.2).

### **Trạng thái đã triển khai (Phase 5 — engine core):**

- **`include/simlab/Cognition.hpp` + `src/Cognition.cpp`** — cognitive layer tách biệt World Truth: `AgentMind` per-NPC giữ `observations` (FIFO cap 64), `beliefs` (key→{status, confidence permille, evidence_for/against, last_tick}), `known_actions`, `known_entities`. `exposed` prop chuyển `domain=observable` — perception CHỈ copy props có domain `observable`; deposit composition/reserve/integrity không bao giờ vào mind.
- **Perception** (`World::perceive`): entity trong `sight_range` → `Observation{entity, tick, pos, observable props}`; self excluded; sight_range=0 → mù.
- **Learning** (`learn_from_event`, gọi tự động trong `interact()` cho actor): ghi `known_actions` từ `CausalEvent.action` (field mới, serialize trong save), outcome beliefs `outcome:<action>:<prop>:<up|down|same>` kèm hướng thay đổi (integer + boolean), confidence +34/lần thành công — belief là evidence, không phải truth.
- **Belief states** đủ 7 mức (§10.2): reinforce → Tested→Supported (≥3 evidence, 0 against); weaken → Disputed (>2× against), confidence clamp 0..1000. Belief không bao giờ ghi vào entity props.
- **Planning** (`plan()`): goal `{prop, value, target}` → quét `known_actions ∩ verbs` × `observations` (chỉ entity NPC đã thấy), đánh giá bằng `law->predict()` — forward model thuần, KHÔNG commit; score theo prediction chạm goal prop; trả ranked `InteractionContext` để executor gọi `World::interact()` thật. NPC chưa biết động từ nào → plan rỗng.
- **Persistence**: minds serialize trong save/load + `state_hash()` (knowledge là sim state vì ảnh hưởng hành vi tương lai).
- **Test (336 checks, 0 fail)**: perception giới hạn range + chỉ observable props (integrity=777/deposit_remaining không lọt vào observation), hai NPC tri thức phân kỳ, NPC mù, belief không mutate truth, học action+outcome từ dig thật, confidence Supported→Disputed, plan() trả dig đúng target và thực thi expose deposit, save/load giữ nguyên mind + hash.

**Giới hạn trung thực còn lại:** planning hiện depth-1 (chưa speculative multi-step qua world-copy); reasoning mới có outcome-belief learning + goal scoring — chưa có forward/backward chaining tường minh, analogical reasoning; witness learning (NPC khác học từ quan sát người khác hành động) chưa có — thuộc Knowledge Transmission (§12); Verified status chưa có cơ chế promote tự động; NPC chưa có needs/motivations (§10.1) nên goal đến từ caller.

**Phase 5 hoàn tất (bổ sung):** needs/motivations đã có — `derive_goals` đọc body-state props (hunger/thirst/fatigue, `domain=need` private) và phát `SatisfyNeed` goals theo urgency; `metabolize`/`dissipate_heat` ledgered qua `World::advance_ticks`. Witness learning: `witness_from_event` — agent trong tầm nhìn học verb + weak outcome/yield evidence (`self=false`, không tính `self_evidence`). Verified promotion tự động: `self_evidence≥150 && evidence_for≥200 && against==0`. Analogical reasoning: material signature (hardness/brittleness/mass lượng tử) — `best_base_belief` transfer kinh nghiệm qua vật liệu tương tự. Multi-step planning: `plan_multi` beam search trên `World::clone()` — candidates qua `interact()` thật trên bản sao, minds trên clone tự học, belief-guided scoring (Supported/Verified thưởng, Disputed phạt). Test: `test_phase5_needs_cognition`.

# **11. Discovery Engine – Hệ thống khám phá không giới hạn danh sách công nghệ**

Trái tim của Open-ended Evolution

Tôi cho rằng đây là module cần được đầu tư nhiều nhất.

Nếu Discovery Engine chỉ tìm trong danh sách công nghệ có sẵn thì Sunnyworld vẫn là một game có cây công nghệ ẩn.

Tôi muốn thay đổi hoàn toàn cách hoạt động.

### **11.1. Discovery bắt đầu từ vấn đề**

Ví dụ:

```
Problem:
NPC bị thú dữ tấn công.

Need:
Survival.

Available Resources:
Stone, Wood, Vine.

Known Knowledge:
Stone can cause physical damage.
Wood can be held and manipulated.

Unknown:
How to construct a more effective weapon.
```

NPC không được nhận thông báo:

`New technology available: Spear.`

Nó phải tìm cách giải quyết vấn đề.

### **11.2. Candidate Generator**

Đây là phần cần thay thế hoàn toàn kiểu Candidate Generator hardcode bằng `if/else`.

Candidate Generator phải có khả năng sinh ra các giả thuyết và chuỗi hành động từ:

- Đặc tính vật liệu.
- Các tương tác đã biết.
- Các tương tác có thể suy ra.
- Những cấu trúc hiện có.
- Nhu cầu NPC.
- Những thất bại trong quá khứ.
- Kiến thức của cá nhân và cộng đồng.

Ví dụ:

```
Candidate 1:
Use stone directly.

Candidate 2:
Use branch directly.

Candidate 3:
Attach stone to branch.

Candidate 4:
Modify stone geometry.

Candidate 5:
Combine multiple branches.
```

Những candidate này là giả thuyết, không phải công nghệ đã được phát minh.

### **11.3. Hypothesis Evaluation**

Không thể thử nghiệm mọi khả năng vì số lượng tổ hợp có thể tăng theo cấp số nhân.

Tôi đề xuất một hàm đánh giá:

\\[ Score(H)= w_1P_s+ w_2P_u+ w_3R- w_4C- w_5U \\]

Trong đó:

- \\(P_s\\): khả năng thành công dự kiến.
- \\(P_u\\): mức độ hữu ích.
- \\(R\\): mức độ giảm rủi ro sinh tồn.
- \\(C\\): chi phí thử nghiệm.
- \\(U\\): độ bất định.

Đây chỉ là mô hình xếp hạng giả thuyết, không phải xác suất vật lý thực sự.

NPC có thể ưu tiên thử nghiệm một giả thuyết có khả năng thành công và chi phí thấp.

Nhưng vẫn có thể thử nghiệm những ý tưởng rủi ro cao nếu hoàn cảnh thúc đẩy.

### **11.4. Experimentation Engine**

Đây là điểm bạn đã xây dựng đúng hướng trong kiến trúc Sunnyworld hiện tại.

Tôi đề xuất giữ nguyên nguyên tắc:

**Discovery → Experimentation → Verification → Transmission.**

Mỗi thử nghiệm phải diễn ra trong thế giới thực.

Ví dụ:

NPC thử buộc đá vào cành cây.

Engine phải tính:

- Thời gian chế tạo.
- Nguyên liệu tiêu hao.
- Sức lực.
- Độ bền liên kết.
- Kết quả khi sử dụng.
- Nguy cơ bị thương.
- Khả năng thất bại.

Không được cho NPC thành công chỉ vì nó đang thử nghiệm một ý tưởng hợp lý.

### **11.5. Discovery Verification**

Một lần thành công chưa chắc đã đủ để chứng minh phương pháp đáng tin cậy.

Tôi đề xuất lưu:

```
struct DiscoveryRecord {
    DiscoveryId id;

    HypothesisId hypothesis;
    std::vector<ExperimentId> experiments;

    std::vector<LawId> supportingLaws;
    std::vector<ConditionId> validConditions;

    ReliabilityEvidence evidence;
    DiscoveryStatus status;
};
```

Ví dụ:

NPC A tạo ra một công cụ thành công một lần.

NPC B thử lại và thất bại vì dây buộc bị đứt.

Engine phải ghi nhận:

- Phương pháp có khả năng hoạt động.
- Kết quả phụ thuộc vào chất lượng dây buộc.
- Cần xác định điều kiện để phương pháp đáng tin cậy.

Đây mới là quá trình khoa học sơ khai.

### **11.6. Không giới hạn số lượng khám phá**

Discovery Engine không nên có:

```
if (discoveryCount > 100)
    stopDiscovering();
```

Thay vào đó, giới hạn phải đến từ:

- Thời gian mô phỏng.
- Năng lực nhận thức.
- Nguồn lực.
- Khả năng thực nghiệm.
- Số lượng giả thuyết được ưu tiên.
- Hiệu năng máy tính.

Đây là khác biệt giữa giới hạn gameplay và giới hạn kỹ thuật.

# **12. Evolution Engine – Tiến hóa sinh học, công nghệ và xã hội**

Tôi đề xuất không xây dựng một Evolution Engine duy nhất xử lý tất cả.

Thay vào đó, tạo một lõi chung về biến đổi và chọn lọc, sau đó chia thành các lĩnh vực.

## **12.1. Biological Evolution**

[What Is Natural Selection Darwin's Theory Quizlet at Charles Bolden blog](https://images.openai.com/static-rsc-4/IvAs0lfs4If10Zg3AS5XIpFVE6HGsqYXW1SVpE4Qr85Zr8Xgz7RrxunYcN9KEdIngqpj7w4glEq1itk3DogDZyLl4QxMImvxwtGnUzHlyaFrZsztb20uxlQq-4ukqlFpBb90U2_TiPciWgtJwZ_X8TLxV1y5jZlAYH9YSwgLE_g?purpose=inline)

[개와 늑대의 시간(L'heure entre chien et loup) : 유전자로 보는 개와 늑대 차이](https://images.openai.com/static-rsc-4/dkoW5dDzDSXi0GOGrlWRdbTZdz_DSl1e_socgH8LPD6kpemQro9apy5hZeRJjPXuybH2EdKTXC59jQx6D0ODDLVr7WYLRdiZ2n2AGic5znwNJ8xiCOdKyoF1rZrC29zLeO_vDm5DhnohREq5UevIfX9sv_Z9kBlxhb18ULPW1is?purpose=inline)

[Geosphere-Biosphere Interactions | University of Tübingen](https://images.openai.com/static-rsc-4/rYMYICjRlt38ZWwM-jW3Oytpo7zNrzD1bZWdvCAEV59KxxZu0m4qLjptrZpYIJZLkxJsro5Bbh8aNZHs4k0yuymKE4f3s3Nk8ZIP-1jUOZnIqeStYNJa36J3dfRhugQBLrmvX1UbcJq7SVU--d820hQ7XEfm8CkMGT1phmqmICM?purpose=inline)

7

Hệ thống cần mô phỏng:

- Di truyền.
- Đột biến.
- Biến dị cá thể.
- Sinh trưởng.
- Sinh sản.
- Cạnh tranh.
- Bệnh tật.
- Chọn lọc tự nhiên.
- Tuyệt chủng.
- Thích nghi môi trường.

Ví dụ:

Một quần thể động vật có nhiều cá thể với các mức tốc độ khác nhau.

Môi trường thay đổi, thức ăn trở nên khó tìm hơn.

Những cá thể có đặc điểm phù hợp hơn có thể sống sót và sinh sản nhiều hơn.

Qua nhiều thế hệ, tần suất các biến thể trong quần thể thay đổi.

Không cần viết:

`if (foodScarcity) increaseAnimalSpeed();`

Đó sẽ là tiến hóa giả.

Thay vào đó, thay đổi quần thể phải xuất hiện từ sự khác biệt di truyền và áp lực chọn lọc.

## **12.2. Technology Evolution**

Công nghệ không nên là danh sách các cấp độ.

Nó nên được biểu diễn dưới dạng các phương pháp đã được khám phá và xác minh.

Ví dụ:

```
Knowledge: Stone Tool Construction

Requires:
- Suitable stone
- Shaping method
- Physical manipulation

Produces:
- A shaped stone object

Properties:
- Geometry
- Sharpness
- Durability
- Mass

Known Limitations:
- Can fracture
- Requires maintenance
```

Một công nghệ mới có thể được tạo ra bằng cách:

- Cải tiến phương pháp cũ.
- Kết hợp nhiều phương pháp.
- Thay đổi vật liệu.
- Thay đổi cấu trúc.
- Tìm ra một quy trình khác.
- Khai thác một đặc tính mới.

Không cần mở khóa công nghệ theo cấp.

## **12.3. Social Evolution**

[Hunter Gatherers Settling by the River | AI Art Generator | Easy-Peasy.AI](https://images.openai.com/static-rsc-4/LLATYMKLkBEXBUaLK_nhBefWTZQZ7aJZZjgb04TTi2qqfw-pVwoWx4S_RkCAb5Ap3QMnvj-Xx6fk2ynXAsk-folql5vj3V1ytVp_QoJfSjEguWODkUAfkOqdZDrrSBp5MS18EclUDaV7f9u4wk_iitOBzE6AjdOPPGm-rX9_nVY?purpose=inline)

[No, our ancestors weren't carnivores. Here's what they really ate and why it matters | BBC Science Focus Magazine](https://images.openai.com/static-rsc-4/F8aJcDIByye5Dj2U0pAm6se0ZjzM4eCVYoTVuyqj1D4NgXnt_lFN3HrCqKMnlvVLpTvjv_tPi9SBKHMnSH6dzjsL8-UPzTci7phtMOmCBtW_togOG4Rg_xObdnvUzW5s2iY68_hkb67SDQMiJRtf1MFA96GNBlDEZaZeFUdun9c?purpose=inline)

[L'apparition des villages : l’exemple de Çatal Hüyük | Lelivrescolaire.fr](https://images.openai.com/static-rsc-4/caL9f9Ojg7MRP99T51HqR4Q3EoRY_B3bgHHTIi0xZGHxgaRB9l-4L13mqbG-A6LaJZMlJyZf4FudSNB8yV8Z-vFt_zX8Y9Od5kv61PlC1o_UmcmTNbeTvDDeOdMcUdvNVuc6DwvX8VlPXIFo3KDzWgJ1QhhtSo2dOCUsTA5w_-Q?purpose=inline)

10

Xã hội phải hình thành từ nhu cầu và tương tác.

Ví dụ:

1. Một nhóm người sống gần nhau.
2. Họ chia sẻ nguồn thức ăn.
3. Nguồn thức ăn trở nên khan hiếm.
4. Xuất hiện tranh chấp.
5. Một số cá thể bắt đầu điều phối việc phân chia.
6. Nhóm hình thành quy tắc.
7. Quy tắc được truyền lại.
8. Xuất hiện vai trò quản lý.
9. Các tổ chức xã hội ngày càng phức tạp.

Engine không cần khai báo sẵn:

`CreateTribe()`

`CreateChiefdom()`

`CreateKingdom()`

Thay vào đó, nó mô phỏng các cơ chế:

- Quan hệ cá nhân.
- Hợp tác.
- Cạnh tranh.
- Quyền sở hữu.
- Trao đổi.
- Phân phối tài nguyên.
- Lãnh đạo.
- Quy tắc.
- Niềm tin.
- Truyền bá văn hóa.

Những cấu trúc xã hội có thể xuất hiện từ sự tương tác của chúng.

Tuy nhiên, cần có một nguyên tắc: tổ chức xã hội phải được biểu diễn như những thực thể có thành viên, quyền hạn, nguồn lực, quy tắc và khả năng thực thi, chứ không phải chỉ là một nhãn `kingdom`.

## **12.4. Cultural Evolution**

Văn hóa cũng cần có cơ chế di truyền riêng:

- Bắt chước.
- Học từ người khác.
- Truyền miệng.
- Giảng dạy.
- Ghi chép.
- Biến đổi kiến thức.
- Thất truyền.
- Khác biệt giữa các cộng đồng.

Một cộng đồng có thể giữ một phương pháp trong hàng trăm năm.

Một cộng đồng khác có thể phát minh độc lập một phương pháp tương tự.

Một phát minh có thể lan rộng, biến đổi hoặc bị lãng quên.

Đây là nền tảng cho lịch sử văn minh không được lập trình trước.

# **13. Knowledge System – Hệ thống tri thức và truyền bá**

Đây là module giúp thế giới không phải liên tục khám phá lại mọi thứ.

Nhưng cũng không được tự động chia sẻ toàn bộ kiến thức cho tất cả NPC.

Tôi đề xuất phân chia tri thức thành ba cấp:

**Individual Knowledge**

Tri thức của từng NPC: trải nghiệm, kỹ năng, phương pháp và những gì họ tin là đúng.

**Community Knowledge**

Tri thức được truyền bá trong một nhóm, bộ lạc, làng hoặc tổ chức.

**Recorded Knowledge**

Tri thức được ghi lại bằng biểu tượng, ngôn ngữ, tài liệu hoặc các hình thức lưu trữ khác.

Một NPC có thể biết chế tạo công cụ nhưng không thể giải thích cách chế tạo cho người khác.

Một người khác có thể quan sát và bắt chước.

Một cộng đồng có thể tổ chức việc truyền dạy.

Một xã hội có thể tạo ra trường học hoặc hệ thống ghi chép.

Tất cả những điều đó phải phát triển từ năng lực và nhu cầu thực tế.

# **14. World History – Lịch sử nhân quả**

Tôi muốn đây là module độc lập.

Mỗi sự kiện quan trọng đều phải có nguồn gốc và liên kết với các sự kiện trước đó.

Ví dụ:

## **Lịch sử hình thành một công nghệ**

1. **Sự kiện 001 – Động vật tấn công**

   Một nhóm NPC bị thú dữ gây thương tích.
2. **Sự kiện 002 – Hình thành nhu cầu**

   Nhóm người cần một phương pháp tự vệ.
3. **Sự kiện 003 – Quan sát đá sắc**

   Một NPC phát hiện đá có thể gây thương tích cho động vật.
4. **Sự kiện 004 – Thử nghiệm**

   NPC thử thay đổi hình dạng và cách cầm đá.
5. **Sự kiện 005 – Khám phá**

   Một phương pháp sử dụng công cụ mới được xác minh.
6. **Sự kiện 006 – Truyền bá**

   Các thành viên khác học được phương pháp.

Đây là một trong những tính năng có giá trị rất lớn cho Sunnyworld.

Bạn có thể mở một vật thể hoặc công nghệ và xem toàn bộ lịch sử hình thành của nó.

Không chỉ xem nó là gì, mà còn biết:

- Ai phát hiện.
- Phát hiện ở đâu.
- Nguyên nhân nào dẫn đến khám phá.
- Những lần thử nghiệm thất bại.
- Ai cải tiến.
- Nó được truyền bá thế nào.
- Nó tác động đến xã hội ra sao.

# **PHẦN III – CÁCH TỔ CHỨC CODE VÀ DATA**

## **15. Kiến trúc thư mục C++**

Tôi khuyến nghị tiếp tục dùng C++ cho lõi simulation của Sunnyworld.

Cấu trúc đề xuất:

```
Sunnyworld/
│
├── engine/
│   ├── core/
│   │   ├── WorldCore
│   │   ├── EntityRegistry
│   │   ├── EventSystem
│   │   ├── SimulationClock
│   │   └── DeterministicRNG
│   │
│   ├── ontology/
│   │   ├── EntityDefinitions
│   │   ├── PropertyRegistry
│   │   ├── MaterialRegistry
│   │   └── RelationshipGraph
│   │
│   ├── resources/
│   │   ├── NaturalOccurrence
│   │   ├── ResourceDeposit
│   │   ├── ResourceDistribution
│   │   ├── ResourceDiscovery
│   │   ├── ExtractionEngine
│   │   └── ResourceDepletion
│   │
│   ├── laws/
│   │   ├── physics/
│   │   ├── chemistry/
│   │   ├── biology/
│   │   └── ecology/
│   │
│   ├── interaction/
│   │   ├── InteractionResolver
│   │   ├── AffordanceEngine
│   │   └── ConstraintSolver
│   │
│   ├── causality/
│   │   ├── CausalEngine
│   │   ├── CausalGraph
│   │   └── EventLedger
│   │
│   ├── processing/
│   │   ├── ProcessRegistry
│   │   ├── ProcessPlanner
│   │   ├── ThermalProcessing
│   │   ├── MechanicalProcessing
│   │   └── SeparationEngine
│   │
│   ├── composition/
│   │   ├── StructureBuilder
│   │   ├── MaterialComposition
│   │   ├── PropertyDerivation
│   │   └── EmergentProperties
│   │
│   ├── agents/
│   │   ├── Perception
│   │   ├── Memory
│   │   ├── BeliefSystem
│   │   ├── Reasoning
│   │   └── Planning
│   │
│   ├── discovery/
│   │   ├── ProblemDetector
│   │   ├── CandidateGenerator
│   │   ├── HypothesisEngine
│   │   ├── ExperimentEngine
│   │   └── VerificationEngine
│   │
│   ├── evolution/
│   │   ├── biological/
│   │   ├── technological/
│   │   ├── social/
│   │   └── cultural/
│   │
│   ├── knowledge/
│   │   ├── IndividualKnowledge
│   │   ├── CommunityKnowledge
│   │   └── KnowledgeTransmission
│   │
│   ├── history/
│   │   ├── WorldHistory
│   │   ├── Provenance
│   │   └── CausalQuery
│   │
│   └── simulation/
│       ├── Scheduler
│       ├── TickPipeline
│       └── LODSimulation
│
├── data/
│   ├── properties/
│   ├── substances/
│   ├── materials/
│   ├── deposits/
│   ├── processes/
│   ├── entities/
│   ├── laws/
│   ├── species/
│   └── environment/
│
├── tests/
│   ├── physics/
│   ├── chemistry/
│   ├── biology/
│   ├── composition/
│   ├── discovery/
│   └── causality/
│
└── tools/
    ├── world_inspector/
    ├── causal_debugger/
    └── discovery_inspector/
```

Đây là cấu trúc logic đề xuất, không có nghĩa mỗi thư mục phải lập tức trở thành một thư viện hoặc module build riêng.

## **16. Data-driven Architecture – Thêm nội dung mà không sửa engine**

Tôi muốn Sunnyworld hướng đến việc thêm vật liệu, loài sinh vật, tính chất và quy luật bằng dữ liệu.

Ví dụ một vật liệu:

```
material:
  id: dry_wood

  composition:
    - substance: cellulose
      proportion: 0.45
    - substance: lignin
      proportion: 0.25
    - substance: water
      proportion: 0.10

  properties:
    density: ...
    moisture: ...
    hardness: ...
    flammability: ...

  structure:
    type: fibrous
```

Các con số ở đây chỉ mang tính minh họa cấu trúc dữ liệu; khi triển khai phải sử dụng thông số có nguồn và đơn vị rõ ràng.

Điểm quan trọng:

Nếu thêm một vật liệu mới mà tất cả đặc tính đều thuộc các mô hình engine đã hỗ trợ, không nên phải viết C++ mới.

Tương tự, mỏ khoáng sản, mỏ quặng mới, quy trình nung/chế biến mới và thực thể sinh học mới đều phải thêm được bằng dữ liệu — xem mục 9.7 và 9.9.

Tuy nhiên, nếu thêm một lĩnh vực khoa học hoàn toàn mới hoặc một mô hình phản ứng chưa được hỗ trợ, việc phát triển một solver mới là hợp lý.

**Data-driven không có nghĩa là engine phải hiểu mọi thứ mà không cần thêm code. Nó có nghĩa là engine không phải viết lại logic chung mỗi lần thêm nội dung.**

# **PHẦN IV – NHỮNG VẤN ĐỀ KỸ THUẬT CẦN GIẢI QUYẾT**

## **17. Simulation phải thực sự xác định và có thể tái hiện**

Tôi đặc biệt khuyến nghị giữ tính deterministic cho simulation chính.

Ví dụ:

Nếu chạy lại thế giới với cùng:

- World seed.
- Trạng thái ban đầu.
- Phiên bản quy luật.
- Thứ tự sự kiện.
- Lịch thực thi.

Thì kết quả phải có khả năng tái hiện.

Điều này giúp:

- Kiểm tra bug.
- Tìm nguyên nhân một NPC chết.
- Tái hiện thí nghiệm.
- So sánh các phiên bản engine.
- Kiểm tra những khám phá ngẫu nhiên.

Không nên dùng một RNG toàn cục tùy tiện.

Nên sử dụng random stream riêng theo entity hoặc sự kiện, kết hợp với thứ tự xử lý ổn định.

## **18. Hybrid Simulation – Mô phỏng nhiều cấp độ**

Không nên mô phỏng tất cả ở cùng một tần suất.

Ví dụ:

| Hệ thống                      | Cách xử lý                           |
| ----------------------------- | ------------------------------------ |
| Va chạm vật thể gần NPC       | Mô phỏng chi tiết                    |
| Phản ứng hóa học đang diễn ra | Cập nhật theo điều kiện và thời gian |
| Sinh trưởng cây               | Cập nhật theo chu kỳ                 |
| Di truyền sinh vật            | Cập nhật theo thế hệ                 |
| Dân số vùng xa                | Mô phỏng tổng hợp                    |
| Lịch sử xã hội                | Cập nhật khi có sự kiện              |

Tôi đề xuất 3 cấp độ:

- **Micro Simulation:** tương tác cụ thể của vật thể và NPC gần người chơi.
- **Meso Simulation:** quần thể, làng, hệ sinh thái và các hoạt động nhóm.
- **Macro Simulation:** dân số, môi trường và biến động văn minh ở quy mô lớn.

Khi người chơi đến gần một khu vực, hệ thống có thể tăng mức độ chi tiết.

Nhưng phải giữ tính nhất quán giữa các cấp độ. Không được để một ngôi làng ở Macro Simulation có quy luật sinh tồn khác hoàn toàn với chính ngôi làng đó khi chuyển sang Micro Simulation.

### **18.1. Mức độ trừu tượng phải được khai báo**

Sunnyworld không mô phỏng từng nguyên tử để tạo thế giới có nhân quả. Mỗi domain dùng mô hình trừu tượng đủ chính xác:

- Địa chất: tầng đá, mạch khoáng, quá trình địa chất.
- Vật lý: lực, chuyển động, va chạm, nhiệt, năng lượng.
- Sinh học: sinh trưởng, di truyền, sinh sản, chọn lọc.
- Xã hội: hành vi, tương tác, tổ chức, truyền bá thông tin.

**Mỗi mô hình phải khai báo giới hạn áp dụng rõ ràng** — engine không được giả vờ hiểu những hiện tượng chưa được mô hình hóa.

### **18.2. Emergence & Complexity Management**

Khi các hệ thống tương tác, thế giới sẽ sinh ra cấu trúc phức tạp ngoài dự kiến (kinh tế, tổ chức, công nghệ). Engine cần quản lý sự phức tạp phát sinh để thế giới không trở thành hỗn loạn hoặc quá tải — kết hợp ngân sách suy luận (mục 19), LOD simulation (mục 18) và ưu tiên hóa candidate (mục 19C). Multi-agent interaction (hợp tác, cạnh tranh, trao đổi, xung đột) là đầu vào chính của emergence xã hội — xem mục 12.3.

## **19. Tránh bùng nổ tổ hợp**

Đây là thách thức rất lớn của Open-ended Evolution.

Giả sử có:

- 100 vật liệu.
- 100 cấu trúc.
- 100 kiểu tương tác.

Không thể thử toàn bộ mọi tổ hợp trong mỗi tick.

Tôi đề xuất:

**A. Affordance Index**

Chỉ tìm những tương tác mà đặc tính của các thực thể có khả năng hỗ trợ.

**B. Dependency Graph**

Theo dõi những đặc tính và điều kiện liên quan đến từng quy luật.

**C. Candidate Prioritization**

Ưu tiên giả thuyết có giá trị và khả thi.

**D. Incremental Evaluation**

Chỉ tính toán lại những gì bị ảnh hưởng bởi thay đổi trạng thái.

**E. Search Budget**

Mỗi NPC có ngân sách suy luận và thử nghiệm riêng.

**F. Shared Knowledge**

Không để 1.000 NPC cùng khám phá lại một phương pháp đã được truyền bá rộng rãi, trừ khi có lý do như thất truyền hoặc thiếu khả năng tiếp cận tri thức.

## **20. Tách Simulation khỏi Rendering**

Đây là yêu cầu kiến trúc bắt buộc.

Simulation không được phụ thuộc vào:

- Sprite.
- Animation.
- Camera.
- Zoom.
- UI.
- Hình ảnh hiển thị.

Một cây trong thế giới phải tồn tại như một thực thể simulation, bất kể nó đang được render bằng sprite nào.

Điều này cũng phù hợp với mục tiêu Sunnyworld chạy được trên máy cấu hình thấp.

### **20.1. World State là Source of Truth duy nhất — Renderer chỉ đọc, không sửa**

Đồ họa không phải là thế giới. Đồ họa là cách biểu diễn dữ liệu của thế giới. Toàn bộ chuỗi phải được chứng minh:

```
World Seed + World Configuration
  → World Generator (địa chất, địa hình, khí hậu, thủy văn, sinh thái, tài nguyên)
  → World State — SỰ THẬT DUY NHẤT (tọa độ, độ cao, vật chất, sinh vật,
    công trình, NPC, trạng thái)
  → Simulation (nhân quả, hành động, tiến hóa, thay đổi thế giới)
  → Renderer (camera, tile, sprite, ánh sáng, animation)
  → Rendered World — phải phản ánh đúng World State
```

Simulation và Renderer **cùng đọc World State**, nhưng Renderer không được tự sửa sự thật của thế giới. Ví dụ bắt buộc: sim đặt núi granite tại `(100,200)` → renderer vẽ núi granite tại đó; sim có 250 cây trong rừng → renderer hiển thị đúng 250 cây; NPC ở trong nhà → renderer không được tự đặt NPC ra ngoài; sông chảy từ cao xuống thấp → renderer thể hiện đúng đường đi, không tự vẽ sông khác.

Renderer được dùng nhiều sprite/hiệu ứng cho một thực thể — không sai, miễn là không làm sai lệch trạng thái mô phỏng.

### **20.2. Kiểm thử ngược Simulation ↔ Renderer**

Phải có bài test dựng World State cố định (núi granite tại `(10,20)`, cao 850; cây sồi tại `(15,20)`; nhà tại `(30,40)`) rồi kiểm tra renderer:

- Thực thể nào bị mất khi render? Thực thể nào xuất hiện mà World State không có?
- Tọa độ có bị sai không?
- Zoom/pan/đổi tầng hiển thị có làm mất dữ liệu không?
- Cây bị chặt trong Simulation → hình ảnh có cập nhật đúng không?

Ba tầng bằng chứng bắt buộc trước khi đầu tư đồ họa: **Data correctness** (worldgen đúng quy tắc), **Simulation correctness** (sự kiện đổi thế giới đúng nhân quả), **Visual correctness** (hiển thị phản ánh đúng dữ liệu). Một bản đồ đẹp vẫn có thể chứa địa hình/sông/tài nguyên sai dữ liệu — không được kết luận đúng chỉ bằng hình ảnh.

Mục tiêu: đổi 2D↔isometric, đổi sprite, đổi phong cách đồ họa **mà không xây dựng lại thế giới** — "thay đổi cách nhìn thế giới mà không phải thay đổi bản thân thế giới."

### **20.3. Kiến trúc render mục tiêu: 2.5D Procedural Terrain & Billboard (Don't Starve-style)**

Pipeline hình ảnh đích khi có renderer — áp dụng nguyên tắc §20.1/§20.2:

**Hai không gian tách biệt:**

- **Simulation Space (mặt phẳng XZ):** collision, pathfinding, spatial partitioning, tương tác vật lý chủ yếu trên `(x, z)`; chỉ query `getTerrainHeight(x, z)` khi cần độ cao/độ dốc (tốc độ di chuyển, độ sâu nước, tầm nhìn).
- **Render Space (XYZ):** map `(x, z)` → `(x, y, z)` với `y = getTerrainHeight(x, z)`.
- **Determinism:** cùng seed + tọa độ → chiều cao, độ dốc, và thuộc tính vật lý mặt đất (độ ẩm, thành phần khoáng, nhiệt độ) giống hệt 100% qua mọi save/load.

**A. Terrain 3D vô hạn theo chunk (CPU):**

- FastNoiseLite (single-header C++) sinh heightfield + data layer môi trường (moisture, mineral density, temperature) theo chunk (32×32 hoặc 64×64).
- Mesh tam giác indexed per chunk: vertex `(x,y,z)`, normal tính sẵn cho lighting/slope, vertex attribute mã hóa **thuộc tính vật lý mặt đất** — không hardcode biome id.
- Streaming động quanh camera: chunk vào vùng active sinh/load bất đồng bộ; chunk ra khỏi cache unload/serialize nếu bị modify.

**B. Terrain shader thủ tục (GPU, SDL3 GPU API hoặc abstraction tương đương):**

- **Không bitmap texture:** mọi màu sắc/grain sinh bằng toán trong fragment shader (noise simplex/fBm/Voronoi — lygia hoặc Ashima).
- **Shading theo thuộc tính & độ dốc:** màu bề mặt suy ra động từ vertex physical properties (moisture, temperature, mineral composition), elevation `y`, và `normal.y`. Độ dốc lớn → đá lộ thiên bất kể độ cao; vùng ngập nước → độ sâu; màu đất dịch theo ẩm và nhiệt độ mùa.
- **Lighting:** directional sunlight (smooth/flat normals) + ambient occlusion cơ bản trong khe địa hình để topology 3D đọc rõ mà không cần texture thủ công.

**C. Entity & prop 2D dạng billboard trong không gian 3D:**

- Quad billboard quay mặt camera, **cylindrical** (khóa trục Y — sprite không nghiêng khi camera pitch).
- Pivot bottom-center neo trực tiếp vào `getTerrainHeight(x, z)`.
- Orbit camera mượt quanh focal point; billboard tự cập nhật hướng.
- Alpha cutout (`discard` dưới ngưỡng) → depth buffer đúng, không sorting artifact sau đồi; đổ bóng hướng lên địa hình 3D chính xác.
- Prop tĩnh/lặp lại (cây, đá, flora) batch bằng GPU instancing.

**Constraints:**

- Pure C++20 + SDL3; không pipeline load texture/mesh ngoài.
- Simulation state **không bao giờ** nằm trong mesh/shader structure — renderer chỉ đọc World State (§20.1).
- Trước khi viết implementation: inspect repo structure và đề xuất cách module tích hợp sạch với setup C++20 + SDL3 hiện có.

**Render validation (bổ sung §22):** renderer phải pass reverse-test §20.2 — cùng World State → cùng hình ảnh về mặt nội dung; chunk streaming/đổi camera không làm mất hay thêm thực thể; thuộc tính vật lý trên vertex phải khớp World Truth (đất ẩm ở cell ẩm, đá lộ ở sườn dốc).

# **PHẦN V – QUY TRÌNH HOẠT ĐỘNG THỰC TẾ**

## **21. Ví dụ hoàn chỉnh: Con người tự phát minh ra vũ khí**

Bây giờ hãy kiểm tra xem toàn bộ kiến trúc trên có hoạt động đúng triết lý Sunnyworld hay không.

Giả sử thế giới mới được tạo.

Ban đầu có:

- Con người.
- Động vật ăn thịt.
- Rừng.
- Đá.
- Gỗ.
- Nước.
- Thời tiết.
- Đói, khát và nhu cầu sinh tồn.

Không có công nghệ vũ khí.

['The egalitarianism of hunter-gatherer societies has often been exaggerated'](https://images.openai.com/static-rsc-4/FWxb5ujTr17HXcmZA-nNh5BRKELq3viw1MExM-xfsz_UyhqlshCnvFOHPPt6X38wjS92SO1gmIYllKEUyKLIuWsaX4yQXrqWVQRQ8a6XMpYmqC3C1hNqHq93xfhCoouMb1Qw4GnFgilQW8dXlLzUGtRHXb9g8j3Y8YspzyE9iI0?purpose=inline)

[Arkeo Blog >  | Arkeoloji ve Sanat - Arkeolojinin Yayınevi](https://images.openai.com/static-rsc-4/l9Qw3tzkhjcuaV3ciS1WjxmFdtRYdaoSuPfPh8ODT8iC0yH7PBC8TnxwrFon5PGZe9IyYHyUt0B884PgT-UYodsDKuNu04i0sMZ2LMkcWqjz6nb0aLON4VytZEvykf4SNF-_8N_QhEN8WUmi2nQmtSuyoywmbjCDqMMoiwpYs1M?purpose=inline)

[Prehistoric humans hunting, artwork by Raul Martin / Msf / Science Photo Library](https://images.openai.com/static-rsc-4/UQ0Tl6hpqP_sEiyWzBmfSCm5-339gC0NTGf8V60QYfYwMhoEuo7ft2owrajupmztikSVxU3v7UlSNMxrGjn5QZ3ltMftN6wx31Idiz4tSjXCb5Rl7qEDyvlKhUYIzQjqvaOSLQaNabGwF8KJsUjPuX26MI05C3IC1UrKIpDmPa0?purpose=inline)

10

## **Mô phỏng quá trình phát minh**

1. **Animal Attack**

   Physics và Biology mô phỏng cuộc tấn công. NPC bị thương. Causal Engine ghi lại nguyên nhân và hậu quả.

   Problem generated
2. **Survival Response**

   Agent Intelligence nhận diện nguy hiểm, hình thành nhu cầu tự vệ và tìm kiếm tài nguyên.
3. **Material Observation**

   NPC quan sát đá, cành cây và những vật liệu xung quanh. Nó ghi nhớ các đặc tính đã quan sát được.
4. **Candidate Generation**

   Discovery Engine sinh ra các giả thuyết sử dụng vật liệu và phương pháp thao tác khác nhau.
5. **Experimentation**

   NPC thử nghiệm. Có thể thất bại, gây lãng phí vật liệu hoặc bị thương.
6. **Composition**

   NPC tìm được cách kết hợp đá, gỗ và dây buộc tạo ra cấu trúc mới.
7. **Verification**

   Công cụ được kiểm tra trong các điều kiện sử dụng khác nhau. Engine xác định giới hạn và độ tin cậy.
8. **Transmission**

   NPC khác học cách chế tạo. Tri thức bắt đầu lan truyền trong cộng đồng.
9. **Further Evolution**

   Công cụ tạo ra nhu cầu mới: vật liệu tốt hơn, kỹ thuật chế tác tốt hơn và phương pháp săn bắt mới.

   New causal cycle

Bây giờ hãy giả sử bạn thêm một loại khoáng vật mới vào thế giới.

Nếu khoáng vật đó có các thuộc tính được Natural Law System hỗ trợ, NPC có thể quan sát, thử nghiệm và tìm ra cách sử dụng nó.

Bạn không cần viết:

```
if (material == NEW_MINERAL) {
    unlock_new_weapon();
}
```

Đây là tiêu chuẩn tôi muốn Sunnyworld đạt được.

## **Ví dụ kiểm chứng thứ hai: NPC muốn có gỗ thì phải tự đi chặt cây**

Đây là bài kiểm tra rất tốt để xác định Sunnyworld đã thực sự đạt kiến trúc mong muốn hay chưa. Giả sử một NPC sống trong rừng, cần xây nơi trú ẩn:

1. **Nhu cầu xuất hiện** – thời tiết lạnh/không có chỗ ở → `Need: Shelter → Goal: Obtain building materials`. Engine không giao nhiệm vụ lấy gỗ.
2. **Tìm hiểu vật liệu** – NPC dựa trên những gì nó biết: đã thấy cây, biết cây có thân/cành, từng cầm cành cây. Nếu chưa biết, phải quan sát hoặc thử nghiệm.
3. **Tìm nguồn tài nguyên** – nhớ khu rừng đã đi, nhìn thấy cây gần, hỏi NPC khác, tổ chức thám dò.
4. **Đánh giá khả năng khai thác** – tiếp cận được không, đủ sức không, có công cụ phù hợp không, có biết cách chặt không, cây có kích thước phù hợp không, có nguy hiểm không. Không công cụ → thử bẻ cành nhỏ, dùng đá, hoặc phương pháp khác; không phải cách nào cũng thành công.
5. **Thực hiện hành động** – engine xử lý lực tác động, độ cứng/cấu trúc gỗ, độ sắc/độ bền công cụ, thời gian/năng lượng, mức tổn thương cây, điều kiện gãy đổ. Không đủ lực thì cây không nhất thiết đổ.
6. **Thế giới thay đổi** – cành gãy, cây tổn thương, đổ xuống, chết hoặc sống tiếp; NPC thu được thân/cành nếu điều kiện cho phép — nhưng đó chưa chắc là gỗ sẵn sàng xây dựng.
7. **Xử lý tài nguyên** – tách cành, cắt đoạn, làm khô, đẽo, vận chuyển; mỗi bước có chi phí và kết quả riêng.
8. **Đạt mục tiêu** – có vật liệu phù hợp → xây dựng. Nếu thành công, NPC ghi nhớ cách làm; NPC khác có thể học.

**Điều quan trọng:** không có dòng code nào bắt buộc NPC chặt cây mỗi khi muốn gỗ. Chặt cây chỉ là một giải pháp NPC có thể khám phá hoặc lựa chọn — nó cũng có thể nhặt gỗ đổ, nhặt cành khô, hoặc trao đổi với người khác.

Engine cũng phải mô phỏng **kết quả ngoài ý muốn**: cây đổ trúng NPC, công cụ gãy, NPC mất sức vô ích, cây trôi xuống sông, khu vực mất cây đổi môi trường, cộng đồng khai thác quá mức làm nguồn gỗ suy giảm, NPC khác tìm ra cách lấy gỗ không cần chặt. Nếu engine chỉ xử lý kết quả NPC mong muốn, nó chưa phải hệ thống nhân quả thực sự.

# **PHẦN VI – NHỮNG NGUYÊN TẮC KHÔNG ĐƯỢC VI PHẠM**

Tôi muốn thiết lập 10 nguyên tắc kiến trúc để tránh Sunnyworld dần biến thành một game hardcode thông thường.

| #  | Nguyên tắc                | Yêu cầu                                        |
| -- | ------------------------- | ---------------------------------------------- |
| 1  | World Truth Independence  | Thế giới tồn tại độc lập với nhận thức NPC     |
| 2  | No Free Knowledge         | NPC không tự biết những điều chưa khám phá     |
| 3  | Causal Consistency        | Mọi thay đổi phải có nguyên nhân               |
| 4  | No Forced Technology Tree | Không phụ thuộc cây công nghệ cố định          |
| 5  | Data-driven Extensibility | Thêm nội dung không phải sửa logic chung       |
| 6  | Emergent Composition      | Cho phép tạo cấu trúc mới từ thành phần        |
| 7  | Evidence-based Discovery  | Khám phá phải được kiểm chứng                  |
| 8  | Knowledge Provenance      | Tri thức phải có nguồn gốc                     |
| 9  | Biological Integrity      | Tiến hóa dựa trên di truyền và chọn lọc        |
| 10 | Deterministic Replay      | Có thể tái hiện và kiểm tra lịch sử simulation |

Có một nguyên tắc bổ sung mà tôi đặc biệt muốn nhấn mạnh:

**Không được tạo ra kết quả chỉ để NPC đạt mục tiêu.**

Ví dụ, NPC muốn có vũ khí nhưng không tìm được vật liệu phù hợp thì nó phải thất bại.

Không được để engine tự tạo ra vật liệu cần thiết chỉ vì NPC đang cần.

Nếu làm được điều này, Sunnyworld mới thực sự có nhân quả.

## **Các invariant cấp engine**

Ngoài 10 nguyên tắc trên, engine còn phải thỏa các invariant sau — đây là quy luật và ràng buộc chi phối cách thế giới thay đổi, không phải trạng thái của thế giới:

| Nguyên tắc | Ý nghĩa |
|---|---|
| Conservation | Không tự tạo vật chất, năng lượng hoặc tài nguyên miễn phí |
| Spatial Consistency | Mọi vật thể có vị trí và quan hệ không gian hợp lệ |
| Temporal Consistency | Hành động và quá trình cần thời gian |
| Physical Constraints | Chủ thể chỉ làm được việc phù hợp khả năng và điều kiện |
| Resource Accounting | Khai thác, chế tạo, tiêu thụ đều phải hạch toán |
| Emergent Behavior | Hành vi phức tạp hình thành từ tương tác, không phải kịch bản |

Lưu ý quan trọng: **không phải mọi quy luật trong thế giới đều là hằng số bất biến.** Nhiệt độ, thời tiết, môi trường và hệ sinh thái luôn thay đổi — cái bất biến là các quy luật và ràng buộc chi phối cách chúng thay đổi.

# **PHẦN VII – KẾ HOẠCH XÂY DỰNG THỰC TẾ**

Tôi không khuyến nghị bạn triển khai cả 12 module cùng lúc. Làm như vậy sẽ rất dễ tạo ra một kiến trúc lớn nhưng các thành phần không thực sự hoạt động với nhau.

Tôi đề xuất 9 giai đoạn.

## **Sunnyworld Development Roadmap**

1. **Giai đoạn 1 – Universal World Foundation**

   Nền móng
   - Entity Registry.
   - Property System.
   - Material System.
   - Natural Resource & Deposit System: occurrence là World Truth data (vị trí, trữ lượng, thành phần, độ sâu, khả năng tiếp cận) — phân biệt Natural Substance / Deposit / Occurrence / Extracted Resource / Processed Material.
   - Observable Evidence properties: mọi deposit/tài nguyên khai báo được dấu hiệu NPC có thể quan sát (màu sắc, lộ thiên, dấu vết) như data thường — không phải truth bị lộ.
   - World State.
   - Relationship Graph.
   - Deterministic Simulation.
   Tiêu chuẩn: thêm vật liệu, thực thể và mỏ mới bằng data mà không sửa World Core; tài nguyên tồn tại trong World Truth trước khi bất kỳ agent nào biết đến. Các phase đầu dùng test-world viết tay; chưa cần worldgen.

   **Trạng thái đã triển khai (Phase 1, audited):** `PropertyDef` có type/unit/range/domain + `uncertainty` (permille), `depends` (prop tiên quyết, enforce ở `create_entity`), `applies`, `provenance` (đường dẫn file). Parser chặt: số phải parse trọn token, token thừa bị từ chối, `depends` phải trỏ prop tồn tại. `load_content` all-or-nothing, lỗi lưu ở `World::content_error`. **Observable Evidence:** `deposits.txt` dòng `evidence <prop> <value>` (chỉ prop `domain=observable`) → copy lên occurrence entity; `perceive` thấy deposit lộ thiên đầy đủ, deposit chôn chỉ lộ đúng evidence (không `exposed`, không composition/reserve), deposit chôn không evidence → vô hình. Deposit: surface ⇒ depth 0, buried ⇒ depth>0. `create_entity` validate trước khi cấp id (không đốt id/kind); `Entity::kind` = kind id thật (>255 kinds ok); `World::find` O(1) qua index, `RelationshipGraph` có index theo entity, `World::relate` từ chối entity chết/không tồn tại. `state_hash` length-prefix chuỗi, gồm `seed`, vị trí các RNG stream và chữ ký content; save v4 lưu `seed` + RNG counters. Giới hạn còn lại: entity chết không được compact; `condition` field của deposit là danh sách cố định trong code (`lithology/elevation/...`); đơn vị (`unit`) vẫn là chuỗi tự do, chưa có UnitId registry. Test: 439 checks.
2. **Giai đoạn 2 – World Generation**
   - Geological / Terrain / Hydrology / Climate / Soil Generators.
   - Geological Process Simulator: tài nguyên hình thành qua quá trình (khoáng hóa, trầm tích, phong hóa), không rải ngẫu nhiên.
   - Resource Distribution Generator: mỗi loại tài nguyên có điều kiện địa lý riêng (xem mục 2.2).
   - Ecology Generator.
   - World Validation: kiểm tra tính nhất quán (không quặng trên cây, không sông chảy ngược dốc, ngoại lệ hợp lý được chấp nhận).
   Tiêu chuẩn: cùng seed + cùng generator version + cùng config tạo ra cùng một thế giới (xem §2.8 — Seed System); thêm loại khoáng sản mới chỉ cần mô tả thành phần + điều kiện hình thành; thế giới tạo ra đủ "thật" để các phase sau kiểm chứng trên đó.
3. **Giai đoạn 3 – Natural Laws & Causality**
   - Physics Model.
   - Material Interaction.
   - Extraction Model: thao tác vật lý tổng quát (đào, đập, tách, chặt) quyết định kết quả theo đặc tính vật liệu — không có action `mine(<material>)` hardcode.
   - Processing Model: quy trình là data (điều kiện nhiệt độ, nhiên liệu, thành phần đầu vào), thất bại khi thiếu điều kiện.
   - Energy & Resource Accounting.
   - Causal Engine.
   - Event Provenance.
   Tiêu chuẩn: cùng một tương tác tạo ra kết quả nhất quán, giải thích được nguyên nhân; quặng không tự biến thành kim loại — mọi chuyển đổi đều qua điều kiện vật lý thực tế.

   **Trạng thái đã triển khai (Phase 3):** `PredictionResult::spawns` + `SpawnSpec` — law models tạo entity mới khi commit; `ExtractionModel` (actions `dig`/`chop`/`strike`, buried→`exposed` gate, yield theo `force×yield_rate` cap `deposit_remaining`, composition shares, fragment tách từ entity thường); `ProcessingModel` + `data/processes.txt` (input/fuel/min_temperature/ticks/output/waste, fail sạch khi thiếu điều kiện, mass balance qua waste); `World::material_inventory()` + `energy_accounting()` (hạch toán từ `energy` prop deltas trong ledger); **energy budget** — prop `energy` (milliJ), mọi action tính cost và trừ actor energy, thiếu năng lượng → fail atomic không mutation; **reach constraint** — prop `reach` (map units), actor-target vượt tầm → fail cho mọi law; **clock consumption** — `PredictionResult::ticks`, `World::interact()` advance `SimulationClock` đúng số tick (process `ticks` là thời gian thật, không chỉ evidence); **`ThermalModel`** (action `heat`) — nhiệt độ tăng theo `E/(100×thermal_mass)`, vật liệu `flammability>0` qua 300°C bắt lửa mất `integrity` — hoàn toàn property-driven, không hardcode tên vật liệu. Thuộc tính mới: `temperature` (deci-°C), `energy` (milliJ), `reach` (map units). Test suite: 201 checks.

   **Phase 3 hoàn tất (audited):**
   - **Điều kiện là dữ liệu:** mọi law ghi `Condition{what, actual, required, met}` (force, reach, energy, temperature, input/fuel) vào `CausalEvent`; `EventLedger::counterfactuals(id)` trả margin + `critical` ("thiếu/đủ sát điều kiện nào thì kết quả đổi") — trả lời §7 câu 3 & 6.
   - **Thử nghiệm thất bại là bằng chứng:** law hiểu action nhưng thế giới từ chối → ghi `CausalEvent{failed, failure, conditions}` (không đổi trạng thái, không tốn thời gian, mind không học từ đó); action không law nào hiểu thì không ghi. `InteractionResult::failed_event`.
   - **Provenance tự động:** `World::touched_` (entity→event cuối chạm vào, dựng lại khi load); `interact` tự gắn `parent_events` = event cuối của mọi target + tool đang cầm (+ caller). `provenance_of(entity)`, `explain_event()` (law theo tên, điều kiện, thay đổi, chuỗi tổ tiên), `ledger.ancestors()`. Quặng→kim loại truy ngược được tới event đào.
   - **Chi phí ≠ hiệu quả:** năng lượng trừ theo *nỗ lực* (force), không theo force đã khuếch đại bởi edge của tool (bug cũ: tool sắc tốn nhiều hơn). **Phản lực (Newton III):** striker đang cầm bị mòn theo độ cứng target (`reaction_wear`), hỏng thì gỡ relation — "cả hai cùng hư hại". Mảnh vỡ thừa hưởng integrity của vật mẹ.
   - **Chế biến:** nhiệt luyện đồng đúng thực tế (min 1000°C = 10000 deci-°C; trước đó 40°C), **hiệu suất theo biên nhiệt** (đúng ngưỡng thu 80%, +20%·min_temp thu đủ) — phần hụt chuyển sang waste (bảo toàn khối lượng, không xóa). Parser: all-or-nothing, số chặt, không ném exception.
   - **Nhiệt & cháy (hóa học tối thiểu, data-driven):** làm nguội Newton về nhiệt môi trường (terrain field, mặc định 20°C) theo `conductivity`, ghi ledger `cool`; cháy bền vững khi ≥300°C (mỗi lần gia nhiệt mất integrity theo flammability), cháy hết → sinh `residue` + `residue_share` (props trên material — không còn tên vật liệu trong code).
   - **Atomic thật:** spawn được validate TRƯỚC khi commit (trước đây spawn lỗi sau khi đã đổi prop → mất input không có output).
   - **Refactor:** `src/LawUtil.hpp` gom reach/energy/wear dùng chung (trước copy-paste mỗi TU). Save v5 (failed/failure/conditions).
   - **Test: 1503 checks.**
   **Giới hạn còn lại:** `metabolize` và `dissipate_heat` quét toàn bộ entity mỗi lần advance (~12 µs/interact ở 1k entity, ~225 µs ở 20k — chưa có index theo prop); chưa có hóa học tổng quát (chỉ cháy; chưa oxy hóa/phân hủy/phản ứng ngoài `processes.txt`); nhiên liệu tiêu thụ chưa quy đổi thành nhiệt (nhiệt nguồn vẫn do prop `temperature` của lò); chưa có sinh học dùng extraction (săn thu thịt/da theo phần) — thuộc Phase 7; ledger tăng không giới hạn.
4. **Giai đoạn 4 – Universal Composition**
   - Structural Composition.
   - Property Derivation.
   - Emergent Properties.
   - Constraint Solver.
   Tiêu chuẩn: tạo được cấu trúc mới mà không khai báo trước sản phẩm cụ thể.

   **Trạng thái đã triển khai (Phase 4):** `CompositionModel` (`physical.compose`) trong `src/CompositionLaws.cpp` — KHÔNG có recipe table A+B→C; constraint solver đánh giá feasibility thuần từ properties (≥2 parts, tất cả trong `reach`, `integrity ≥ 20`, tổng `mass ≤ max_load` của actor, energy cost = 500 + mass/100, `≤ 8` parts, cấm nest composite và part đã `contained`). **Cấu trúc là thật**: parts giữ nguyên alive + prop `contained`, liên kết composite bằng `part_of` relation edges qua `SpawnSpec::rel_from`/`RelationChange` — relation mutations là causal change hạng nhất trong `PredictionResult`, commit atomic cùng prop changes, vào ledger evidence (`rel+/rel-`) và `state_hash()`/save-load. `disassemble` đảo ngược hoàn toàn: destroy composite, `contained→false`, gỡ `part_of` edges, parts khôi phục đúng props gốc. Mọi law (physics/extraction/processing/thermal/composition) đều skip target `contained`. Property derivation theo nhóm (§8.2): sum (mass/thermal_mass/length), min (integrity — weakest link), max (flammability/edge), mass-weighted avg (hardness/brittleness/conductivity/temperature). Emergent (§8.3): `thrust_range` chỉ tồn tại trên composite khi có edge+length — và **được consume thật**: actions `wield`/`unwield` tạo/gỡ quan hệ `wielding` (actor→tool), `World::tool_bonus()` trả (max thrust_range, max edge) trong số các tool đang cầm; Physics/Extraction dùng thrust_range làm reach bonus và edge làm force multiplier (`E×(1000+edge)/1000`), Thermal/Processing được reach bonus. Structural invariant trong `World::interact`: entity chết → tự động gỡ mọi relation edge chạm nó, giải phóng `part_of` parts (`contained→false`), rơi `wielding` — không còn part mồ côi. Props mới: `length`, `edge`, `thrust_range`, `made_of`, `contained`, `max_load` (actor-side, optional). §8.4 Biological Composition thuộc Phase 7. **Parametric Physical Geometry (§8.5) đã triển khai lõi:** geometry là STATE — props `geom_type` (none/rod/plate/blob) + `geom_len/wid/thk`; `materials.txt` chỉ khai báo default cho nguyên liệu thô, còn `carve` (rod→ngắn dần, plate→mỏng dần, blob→thu đều) và fragment-spawn scale đổi hình dạng **động trong runtime**, mass scale theo volume ratio (bảo toàn). `src/Geometry.cpp::geom_stats()` suy ra derived quantities: volume/surface proxies, mass-weighted CoM, inertia `Σm·d²` từ grip point, `swing` (effective reach từ primitive extent), `floats` (density < nước). Composite tự aggregate geometry của `part_of` parts theo vị trí thật. `tool_bonus` reach = max(thrust_range, geom swing) — giáo dài tự với xa hơn **không cần tag**. Test: 281 checks.
   **§8.5 tiếp tục:** `geom_orient` (turn permille, integer sin/cos 32-step table — deterministic, không float) — rod/plate xoay thật trong `geom_stats` (CoM/swing theo hướng). **Geometric fit constraint**: assemble giờ check pairwise bind-distance — hai part phải gần nhau *tương đối với kích thước* (`bind_r = max-dim/2` mỗi part + gap 60); hai cục nhỏ cách 200u không buộc được dù trong reach của actor. Test: 286 checks.
   **§8.5 socket & joint strength hoàn tất:** `socket_points()`/`socket_gap()` trong `src/Geometry.cpp` — sockets DERIVED từ primitive (rod: 5 điểm dọc thân, plate: 4 cạnh + tâm mặt, blob: tâm), xoay theo `geom_orient`, composite = union sockets của parts. Fit check giờ là **socket-to-socket distance ≤ 100** — bề mặt phải gần chạm nhau thật sự. **Joint strength theo binding type**: `note` của assemble chọn `bind|wedge|pierce|balance` (factor 0.8/0.9/0.6/0.4), joint = min(`tensile` của cặp part) × factor — `tensile` là prop mới trên materials; composite integrity = min(part integrity, mọi joint) → cấu trúc gãy ở mắt xích yếu nhất, `pierce` yêu cầu edge ≥ 300, `joint_type` ghi vào composite làm provenance. 2.5D renderer consume primitives = phase render (ngoài phạm vi engine). Test suite: 299 checks.
5. **Giai đoạn 5 – Agent Intelligence**
   - Perception: World Truth → Observation theo phạm vi cảm nhận và điều kiện lộ thiên; NPC không đọc trực tiếp deposit/truth.
   - Memory.
   - Belief: ghi nhận observation và đánh giá độ tin cậy, không ghi truth.
   - Reasoning.
   - Planning.
   Tiêu chuẩn: NPC chỉ hành động theo thông tin nó có thể tiếp cận; hai NPC có trải nghiệm khác nhau hình thành kiến thức khác nhau; NPC có thể suy luận sai từ dấu hiệu.
6. **Giai đoạn 6 – Discovery Engine**
   - Problem Detection.
   - Candidate Generator.
   - Resource Hypothesis & Purposeful Survey (tìm kiếm tài nguyên không cần nhiệm vụ lập trình sẵn).
   - Hypothesis.
   - Experimentation.
   - Verification.
   Tiêu chuẩn: NPC tự tìm ra phương pháp mới qua thử nghiệm thực tế; di chuyển mỏ hoặc che phủ/khai thác cạn tài nguyên làm thay đổi nhận thức và kết quả tương ứng.

   **Trạng thái đã triển khai (Phase 6 core):** `include/simlab/Discovery.hpp` + `src/Discovery.cpp`.
   - **Problem Detection** (`detect_problems`): unmet needs từ `derive_goals` + `SuspiciousSign` — observation chứa observable props ngoài `exposed` (vết khoáng surface_color/surface_trace) → vấn đề cần điều tra; caller goals qua `extra`.
   - **Candidate Generator** (`generate_hypotheses`): need/extra goals → `plan_multi` chains (đã belief-guided); suspicious signs → single-step probes với mọi known verb mà law `predict()` cho possible — purposeful survey không cần nhiệm vụ lập trình sẵn. Score(H) §11.3 integer permille: `Ps` (dự đoán/plan score), `Pu` (goal progress), `R` (urgency), `−C` (ticks+steps), `−U` (uncertainty theo DiscoveryRecord status).
   - **Experimentation** (`run_experiment`): thực thi plan steps qua `World::interact` trên World THẬT — law quyết định kết quả; step fail/refused (`failed_event`) vẫn là evidence, plan dừng sớm. Verdict đo từ World Truth deltas (need giảm, spawn/exposed/deposit_remaining đổi trên subject, goal prop đạt) — không phải từ intent.
   - **Verification** (`DiscoveryRecord` trong `AgentMind::discoveries`): experiments event ids, supporting_laws (tên law đã fire), evidence_for/against, trials, status `Open→Tested→Supported→Verified|Refuted` (Verified cần trials≥6 + for≥150 + 0 against — thành công cá nhân lặp lại). Persist trong save v6 + `state_hash()`.
   - `discover_step` = một chu kỳ đầy đủ: detect → generate → chọn hypothesis điểm cao chưa Verified → chạy experiment thật; khi không có hành động tại chỗ nào khả thi, trả `ExploreGoal{target,subject,reason}` làm waypoint cho lớp locomotion/controller bên ngoài (Discovery không tự dịch chuyển NPC).
   - **Phase 6 completion (đã đóng 6 lỗ hổng):**
     - *MethodKey theo signature, không gắn EntityId:* target signature = lượng tử hóa material (hardness/brittleness/mass theo §10.3) cho entity có material, hoặc observable-sign profile (prop+giá trị lượng tử theo range) cho entity chỉ nhìn thấy (deposit). Kinh nghiệm "dig vết xanh" trên mỏ A áp dụng cho mọi mỏ cùng dấu hiệu; `sample_entities` chỉ là provenance (cap 8), không phải khóa.
     - *validConditions + mở khóa Refuted:* `DiscoveryRecord::failed_conditions` gom các `Condition{what,actual,required,met=false}` từ `failed_event` (giữ ngưỡng chặt nhất theo `what`). `conditions_unlocked` so predicted conditions của candidate mới với các biên đã thất bại — đủ lực/nhiệt/tầm với/reach mới thì method tự mở lại (`refuted_unlocked`), vẫn thiếu thì bị chặn offer. Persist save v7 + `state_hash()`.
     - *Exploratory Synthesis:* khi plan_multi không ra nghiệm hoặc gặp Threat/CapabilityGap, `generate_hypotheses` sinh candidate `assemble` theo cặp entity trong tầm nhìn (predict_assemble quyết định khả thi) + `heat`/`strike` transforms — tất cả qua `predict()` và `known_actions`, không tên vật liệu/công nghệ. Search budget §19E: pool ≤8 entity, ≤6 candidate khả thi mỗi problem.
     - *Threat & CapabilityGap:* `detect_problems` quét 32 event gần nhất của ledger — integrity của NPC giảm → `Threat` (subject = participant gây hại); `failed_event` có condition unmet thuộc `force`/`reach`/`energy` → `CapabilityGap`. Đây là ngòi nổ tự nhiên kích hoạt synthesis, không kịch bản "bị cắn → chế giáo".
     - *ExploreGoal:* sign/threat ngoài `reach` → waypoint thay vì probe bất khả thi.
   - **Test:** `test_phase6_discovery` (core) + `test_phase6_completion` (signature reuse, conditional refutation, synthesis budget, explore waypoint) + `test_phase6_selfdefence` — kịch bản §21 trọn vẹn: NPC bị impact làm giảm integrity → detect Threat + CapabilityGap(reach) → synthesis đề xuất assemble hai vật liệu thường → experiment thật tạo composite có thrust_range/edge/integrity → wield → impact trả đũa thành công. Không một tên "spear"/"wolf" nào trong engine hay test.
   **Giới hạn còn lại:** locomotion là lớp ngoài (Discovery chỉ phát waypoint — đúng tách tầng); chưa có `teach`/Recorded Knowledge (Phase 8 — tri thức thất truyền khi người biết chết là động lực đúng, không bị che); NPC không được "cứu" bằng code — thất chủng là dữ liệu môi trường cần tinh chỉnh, không phải bug engine.
7. **Giai đoạn 7 – Biological Evolution**
   - Genetics.
   - Reproduction.
   - Mutation.
   - Natural Selection.
   - Ecology.
   Tiêu chuẩn: quần thể thay đổi qua nhiều thế hệ mà không cần script tiến hóa.

   **Trạng thái đã triển khai (Phase 7 core):** `Genome` (Entity.hpp) — hereditary loci `PropId → allele` sorted, World Truth không phải agent knowledge; expression là identity cho numeric traits (allele == prop value). `SpeciesDef.genes` + parser `gene <prop>` trong `species.txt` — locus nào di truyền là DATA, engine không hardcode tính trạng. Props mới: `age`, `adult_age`, `mutation_rate` (domain biological). `BiologyModel` (`biology.reproduction`, law id 7) trong `src/BiologyLaws.cpp` — action `"reproduce"` actor+1 partner; compatibility CẤU TRÚC (cùng `kind_name` + cùng tập locus sorted — không tra bảng species); gates: maturity (`age >= adult_age`, default 100), fertility >0 permille cả hai, hunger < 900 (cha mẹ đói không sinh sản — áp lực chọn lọc, không phải rule), reach, energy cost 2000 milliJ. Heredity: mỗi locus chọn allele của một cha/mẹ qua hash thuần của (world_seed, parent ids, tick, locus) — deterministic, clone-safe, KHÔNG tiêu tốn RNG stream; mutation_rate permille của actor, delta hash-derived kẹp trong range khai báo của property. Offspring spawn qua `SpawnSpec.genome` (commit trong `World::interact` cùng atomic spawn), needs zeroed, relations `parent_of` cả hai phía. `metabolize` mở rộng: genome carriers tích lũy `age` theo thời gian; need ≥950 → mất integrity theo metabolism (đói là áp lực chọn lọc cổ xưa nhất); integrity 0 → reap (`alive=false`, gỡ spatial) qua cùng ledger `metabolize`. WorldGen `stage_ecology`: founding population có genome với deviation tất định theo (cell, species, locus) — quần thể khởi đầu đã có biến dị di truyền, genome == phenotype lúc sinh. Hash/save-load: genome vào `state_hash()`, `content_hash()`, save v8.
   **Test:** `test_phase7_biology` — reproduce thành công (offspring genome recombine trong range, phenotype==allele, energy cost, parent_of relations, age tăng theo tick); mọi refusal (kind khác, genome thiếu locus, nonadult, sterile, starving, out-of-reach, thiếu energy) đều failed_event causal; mutation_rate=1000 → biến dị allele qua 12 thế hệ con + replay bit-identical (`state_hash` bằng nhau); genome roundtrip qua save/load v8 (`state_hash` bằng nhau); **selection nổi sinh**: hai biến thể cùng kind khác chỉ metabolism allele — đói trường không có thức ăn → biến thể metabolism thấp sống, biến thể cao chết vì integrity 0, không một dòng code nào nói allele nào "tốt".
   **Bổ sung (đóng 2 gap):**
   - *Locus divergence → speciation:* structural mutation trong recombination — `mutation_rate/20` permille cơ hội mỗi lần sinh: LOSS xóa một locus bất kỳ (không bao giờ locus cuối), GAIN thêm một locus từ gene pool của species (`species.txt` `gene` lines, trừ locus đã có; allele ngẫu nhiên tất định trong range). Vì compatibility đòi hỏi locus set GIỐNG HỆT, lineage lệch locus bị cách ly sinh sản khỏi quần thể gốc — loài mới nảy sinh thuần túy từ divergence, không bảng species nào can thiệp. Test F: 120 lần sinh với mutation_rate=1000 → mutant xuất hiện → giao phối được với partner cùng locus set, bị từ chối với parental set.
   - *Food chain / carcass consumption:* `consume` (physiology) giờ chấp nhận target đã chết — xác chết là edible remains theo `nutrition`/`hydration` World Truth (flesh: 700/300). Carcass bị strip (nutrition→0) sau khi ăn hết nên không thể cho ăn lặp lại; carcass có `quantity` cho ăn theo đơn vị. Săn mồi cần KHÔNG code mới: `impact` giết → `consume` ăn — cùng action generic mọi sinh vật đều có. Test G chứng minh kill→meal→strip.
   **Giới hạn còn lại:** chưa có mate choice / parental care / mate guarding (để Phase 8 — dùng `parent_of` + social relations `trust`/`affinity` làm nền, không hardcode hành vi nuôi con trong BiologyLaws); chưa có §8.4 Biological Composition; ecology chưa có predator-prey dynamics thật (chưa có động cơ săn — NPC/controller tự quyết định impact ai), carrying capacity, hay succession; GAIN locus chỉ lấy từ gene pool của species đã khai báo (không phát sinh prop hoàn toàn mới — cần cơ chế "gene mới" nếu muốn trait chưa từng thấy); chưa có lão hóa/senescence (age chỉ là counter — chưa có tuổi thọ tối đa).
8. **Giai đoạn 8 – Social & Cultural Evolution**
   - Cooperation.
   - Organization.
   - Resource Distribution.
   - Social Rules.
   - Knowledge Transmission.
   Tiêu chuẩn: tổ chức và thiết chế xã hội hình thành từ nhu cầu thực tế.

   **Trạng thái đã triển khai (Phase 8 core):** `SocialModel` (`social.interaction`, law id 8) trong `src/SocialLaws.cpp` + `social_from_event` trong `src/Cognition.cpp` — bảy verb generic, KHÔNG có class Tribe/Kingdom/Religion/Quest nào trong C++.
   - *Weighted social ties:* `World::social_` = map `Relation{a,type,b} → weight [-1000,1000]` — cạnh "trust"/"debt"/"authority" là World Truth về quan hệ, không phải belief. `tie_weight`/`adjust_tie` (clamp), purge khi entity chết, hash + save v9 + clone.
   - *Knowledge transmission:* `communicate` (trong `communicate_range`, tốn energy/ticks) — post-commit, các DiscoveryRecord ở trạng thái Supported/Verified của actor chảy sang mind đối tác dưới dạng claim **Open chưa kiểm chứng** (evidence/trials/self = 0 — phải tự verify lại trong World thật), kèm `failed_conditions` (ranh giới thất bại đã trả giá) và `supporting_laws`. Số claim truyền tỉ lệ với `trust` tie (0→1, 500→3, 1000→5); `known_actions` truyền hết (cap 16). `read` đọc `inscription` prop tương tự.
   - *Recorded Knowledge:* `engrave` khắc một record Supported+ của actor (chọn bằng `note`=key hoặc tự động record tốt nhất) thành text prop `inscription` trên entity bất kỳ (định dạng `R|key|status|efor|eagainst|trials|claim`, cap 8000 chars) — tri thức vượt qua cái chết sinh học vì nó là World Truth khắc trên đá. `read` cho NPC khác học lại claim.
   - *Institutions as entities:* `found` spawn entity `collective` (prop `collective=true`, `quantity=0` làm kho chung) + `member_of` edge; `join`/`leave` quản lý membership; `share` chuyển toàn bộ `quantity` của actor vào kho (tặng/thuế cống — ý nghĩa do caller quyết). **Rules là data**: collective mang prop `taboo_harm` → mọi event gây giảm integrity giữa hai member kích hoạt phạt trust (-100 mỗi member, -200 nạn nhân) qua `social_from_event` — cơ chế enforcement generic, rule mới = prop mới + check mới.
   - *Trust dynamics:* `share` tăng trust hai chiều +50; vi phạm taboo giảm trust; hòa giải/uy tín có thể nối `authority`/`debt` sau này trên cùng map.
   **Test:** `test_phase8_social` — communicate truyền Verified→Open claim đúng giới hạn trust (0→1 record, 1000→nhiều), failed_conditions đi kèm; engrave→read roundtrip + tác giả chết record vẫn đọc được; found/join/share/leave + taboo penalty (-100/-200) và outsider không bị phạt; determinism + save/load v9 (`state_hash` bằng nhau).
   **Bổ sung (đóng nốt debt/authority + ration):**
   - *debt:* `share` cho target đang needy (`hunger>500` hoặc `integrity<500`) → receiver `debt`→giver +150; debtor share lại cho creditor → debt −150 (trả quá thì đảo chiều — negative debt là claim ngược). Trao đổi/ân huệ tạo nghĩa vụ tự nhiên.
   - *authority:* `communicate` truyền ≥1 record MỚI (learner chưa có key) → learner `authority`→teacher +100; actor đánh entity T mà T từng gây hại B trong 32 event gần nhất → B `authority`→actor +100 (thủ lĩnh = người biết nhiều + dũng cảm, không bầu cử).
   - *ration/tribute:* prop `ration_share` (permille) trên collective — `World::social_tithe` chạy trong commit của MỌI interact: member harvest (spawn entity có `nutrition>0`+`quantity`, hoặc actor quantity tăng) bị trích `gained×share/1000` vào kho chung `quantity` của group, cùng causal event. Verb `withdraw` mới: member+`hunger>700` rút sạch kho không phạt; member no đói −25 trust; outsider = trộm → mọi member trust→actor −100. Test C2/C3 chứng minh đầy đủ.
   **Giới hạn còn lại:** beliefs chưa truyền (chỉ DiscoveryRecord + known_actions); `teach` chưa có curriculum/observe-then-imitate chain; tithe chỉ tính phần `quantity` (item nguyên không chia được); withdraw lấy TOÀN BỘ kho (không có hạn mức — enforcement xã hội lo, không engine); luật vẫn chỉ `taboo_harm` + ration; language chưa có symbol emergence; institutions chưa tự hành động tập thể.
9. **Giai đoạn 9 – Open-ended World Validation**
   - Performance Optimization.
   - Multi-scale Simulation.
   - World History.
   - Long-term Simulation Tests.
   - Emergent Behavior Analysis.
   Tiêu chuẩn: thế giới tiếp tục tạo ra những chuỗi tiến hóa mới mà không cần thêm kịch bản riêng.

   **Trạng thái đã triển khai (Phase 9 — LOD foundation + World History):**
   - *Dormant-entity metabolism LOD:* `Entity::last_metabolized` (World Truth, save v10, `state_hash`). `World::metabolize` chia đôi — `drift_needs()` là hàm drift duy nhất cho cả hai chế độ: entity trong vòng `kLodRadius=1024` của một agent (hoặc chính agent) được cập nhật mỗi tick; entity xa hơn là DORMANT, bỏ qua tới khi tích lũy `kDormantStride=8` tick rồi catch-up một lần với `dt=pending`. Needs tuyến tính (cap 1000 hấp thụ), age tuyến tính, và starvation được đếm THEO TỪNG TICK trong cửa sổ catch-up (`first_starving` tính tick đầu tiên need ≥950) → props bit-identical với per-tick, LOD chỉ đổi chi phí không đổi luật. integrity 0 reap NGAY trong cùng event (không chờ stride sau). Dormant mode chỉ bật khi `minds_` không rỗng — trước khi có agent không có "xa/gần" nên drift giữ fine-grained.
   - *Consistency tại biên quyết định:* `World::flush_needs` được gọi đầu `interact()` cho actor+targets — participant dormant được flush pending drift TRƯỚC khi law predict, nên `share`/`reproduce` phán xét trên needs hiện tại chứ không phải stale. Kể cả interact bị refuse cũng flush (thời gian đã trôi qua là causal fact).
   - *Material prefilter:* `mat_need_flags_` — vật liệu không cung cấp bất kỳ prop need/metabolism/integrity/age nào bị skip chỉ bằng 1 lookup mảng trước mọi prop lookup; genome carriers và instance-prop entities vẫn được kiểm tra đầy đủ.
   - *World History:* `World::history_of(id, limit)` → EventId mới→cũ chạm entity (participant/change/spawned), resolve qua `ledger.find()` — Id ổn định khi ledger lớn (tránh con trỏ dangling vào vector grow).
   **Test:** `test_phase9_world` — dormant entity hunger 0→16 sau đúng 2 stride (bằng per-tick), starvation mid-window chỉ trừ 4/8 tick damage (460 chứ không phải 420), flush_needs qua refused interact, history_of ordering + metabolize inclusion + entity không liên quan rỗng, clone-giữa-chừng + save/load v10 + 837-tick deterministic replay hash bằng nhau.
   **Giới hạn còn lại:** `dissipate_heat` vẫn per-tick (geometric cooling không batch tuyến tính được — `dt>2000` đã có shortcut ambient); chưa có Meso/Macro population-level aggregation thật (chưa có carrying capacity, census, hay entity-non-agent nào ngoài organism); `active` set rebuild mỗi metabolize call (O(minds×neighbors) — chấp nhận được ở quy mô hiện tại); chưa có emergent-behavior analyzer tự động (mới có infrastructure chứng minh chuỗi tiến hóa chạy deterministic).

## **22. Những bài kiểm tra bắt buộc**

Tôi muốn mỗi giai đoạn có các bài kiểm tra tự động. Không nên chỉ kiểm tra bằng cách chạy game và nhìn xem NPC có hành động hay không.

| Test                   | Điều cần chứng minh                                         |
| ---------------------- | ----------------------------------------------------------- |
| Add New Material       | Thêm vật liệu không sửa engine                              |
| Add New Property       | Thuộc tính mới được hệ thống hỗ trợ                         |
| Add New Deposit        | Thêm mỏ/khoáng sản mới bằng data, không sửa engine          |
| Resource Existence vs Knowledge | Tài nguyên tồn tại trong World Truth nhưng agent không tự biết vị trí/thành phần |
| Extraction by Physics  | Khai thác là thao tác vật lý tổng quát; lực không đủ thì thất bại, không có tài nguyên miễn phí |
| Processing Conditions  | Chế biến thất bại khi thiếu nhiệt độ/nhiên liệu/thành phần phù hợp |
| Perception Boundary    | NPC không biết vị trí/thành phần mỏ khi chưa có bằng chứng; không khai thác được tài nguyên ngoài khả năng tiếp cận |
| Divergent Knowledge    | Hai NPC có trải nghiệm khác nhau hình thành kiến thức khác nhau; có thể suy luận sai từ dấu hiệu |
| Relocation/Depletion   | Di chuyển mỏ, che phủ hoặc cạn kiệt tài nguyên làm thay đổi nhận thức và kết quả khai thác |
| World Consistency      | Worldgen không tạo tài nguyên vô lý (quặng trên cây, sông ngược dốc); ngoại lệ hợp lý được chấp nhận |
| Unknown Interaction    | NPC chưa biết tương tác đó                                  |
| Independent Discovery  | Hai NPC có thể tự khám phá độc lập                          |
| Failed Experiment      | Thất bại có hậu quả thực tế                                 |
| Knowledge Transmission | Tri thức được truyền qua hành vi                            |
| New Composition        | Tạo cấu trúc mới từ thành phần                              |
| Biological Inheritance | Tính trạng được di truyền                                   |
| Natural Selection      | Quần thể thay đổi qua thế hệ                                |
| Causal Replay          | Tái hiện được chuỗi sự kiện                                 |
| Goal-Driven Action     | NPC muốn gỗ tự lập kế hoạch khả thi; không có cây gần → tìm nguồn khác hoặc thất bại; không biết chặt → không tự có kỹ năng |
| Tool & Condition Dependence | Công cụ hỏng/yếu làm thay đổi kết quả hành động; hành động chịu giới hạn vật lý |
| Emergent Divergence    | Hai cộng đồng cách biệt có thể phát triển phương pháp khác nhau; nhu cầu mới tạo giải pháp chưa lập trình |
| Long-term Simulation   | Thế giới không cần kịch bản định sẵn để tiếp tục phát triển |
| Rendering Fidelity     | Renderer không tự thêm/xóa/dời thực thể; hiển thị đúng World State (xem §20.2) |
| Simulation-Render Sync | Thay đổi trong Simulation (chặt cây, xây nhà, NPC di chuyển) phản ánh chính xác trên hình ảnh |

Một bài kiểm tra đặc biệt quan trọng:

**Hãy tạo một thế giới mà không có công nghệ, không có cây công nghệ, không có danh sách phát minh. Sau đó để NPC sinh tồn trong hàng nghìn năm mô phỏng.**

Nếu NPC có thể tự khám phá, tạo phương pháp mới, truyền bá kiến thức và hình thành những tổ chức mới dựa trên các quy luật mà engine hỗ trợ, đó là dấu hiệu kiến trúc đang đi đúng hướng.

## **Bài kiểm thử tổng hợp Open-ended Evolution**

Một thế giới hoàn toàn mới được tạo ra — không công nghệ, không kiến thức khai thác, không hướng dẫn sinh tồn. NPC phải có khả năng, **nếu điều kiện cho phép**:

1. Nhận biết môi trường.
2. Phát hiện nguồn nước và thức ăn.
3. Khám phá tài nguyên tự nhiên.
4. Tìm ra cách thu nhận và xử lý tài nguyên.
5. Tạo ra công cụ.
6. Sử dụng công cụ để giải quyết vấn đề.
7. Xây dựng nơi trú ẩn.
8. Học hỏi và truyền đạt kiến thức.
9. Hình thành hợp tác và tổ chức xã hội.
10. Để lại kiến thức, lịch sử và môi trường biến đổi cho thế hệ tiếp theo.

Không cần mọi thế giới đều đạt đến bước 10. Điều quan trọng: **không có bước nào được đảm bảo bằng kịch bản hoặc công nghệ được mở khóa sẵn.**

# **PHẦN VIII – HƯỚNG ĐI TÔI KHUYÊN DÙNG CHO SUNNYWORLD HIỆN TẠI**

Tôi muốn liên hệ trực tiếp với kiến trúc bạn đã có.

Trước đây Sunnyworld đã có một số thành phần:

- World Truth tách biệt Agent Belief.
- Discovery → Experimentation → Verification → Transmission.
- Resource và environmental simulation.
- Causal records và provenance.
- Candidate Generator.
- Một số hệ thống xã hội như granary, wardens và religion.

Tôi không khuyến nghị viết lại toàn bộ dự án.

Thay vào đó, cần thực hiện một cuộc **Architecture Evolution**, chuyển những hệ thống hiện có thành các module tổng quát.

| Hệ thống hiện tại   | Hướng phát triển                   |
| ------------------- | ---------------------------------- |
| Nature.cpp          | Natural Law Registry và các solver |
| Conditions.cpp      | Property/State System              |
| Candidate Generator | General Hypothesis Generator       |
| Discovery           | Discovery Engine tổng quát         |
| Experimentation     | Universal Experiment Engine        |
| Verification        | Evidence-based Verification        |
| Transmission        | Knowledge Network                  |
| Causal Records      | Causal Engine và Event Ledger      |
| NPC                 | Cognitive Agent                    |
| Resource System     | Natural Resource & Extraction System (Occurrence → Extraction → Processing) |

Đặc biệt, cần kiểm tra lại mọi đoạn code kiểu:

```
if (problem == WATER_SHORTAGE) { ... }

if (problem == FOOD_SHORTAGE) { ... }

if (problem == ANIMAL_ATTACK) { ... }
```

Không phải mọi `if` đều xấu. Các solver chuyên biệt vẫn cần điều kiện cụ thể để thực hiện mô hình khoa học.

Vấn đề là không được để Candidate Generator, Discovery Engine và Evolution Engine phải biết trước từng loại vấn đề hoặc từng kết quả.

# **PHẦN IX – QUYẾT ĐỊNH KIẾN TRÚC CUỐI CÙNG**

Tôi muốn chốt một số lựa chọn kỹ thuật:

| Hạng mục                | Đề xuất                                      |
| ----------------------- | -------------------------------------------- |
| Ngôn ngữ lõi            | C++                                          |
| Simulation architecture | Data-oriented + ECS                          |
| World rules             | Modular domain solvers                       |
| Interaction             | Affordance + constraint resolution           |
| Causality               | Event ledger + causal graph                  |
| Composition             | Structural graph + property derivation       |
| NPC intelligence        | Symbolic reasoning + planning                |
| Discovery               | Hypothesis generation + experimentation      |
| Biological evolution    | Genetics + population simulation             |
| Social evolution        | Agent interaction + institutional emergence  |
| Data                    | Declarative definitions                      |
| Determinism             | Seeded deterministic simulation              |
| Performance             | Multi-scale simulation + incremental updates |
| World generation        | Process-based (geology → terrain → climate → ecology → resources) |
| Agent planning          | Goal decomposition + constraint search over known actions |

## **Kiến trúc 5 tầng mục tiêu (bản chốt)**

| Tầng | Thành phần |
|---|---|
| **LAYER 1 – World Foundation & Natural Laws** | Không gian, thời gian, địa chất, vật lý, hóa học, sinh học, khí hậu, sinh thái |
| **LAYER 2 – Causality, Constraints & Processes** | Nhân quả, tương tác, affordances, bảo toàn tài nguyên, biến đổi vật chất, quá trình tự nhiên |
| **LAYER 3 – Agent Intelligence** | Nhận thức, ký ức, tri thức, nhu cầu, động lực, suy luận, lập kế hoạch, thử nghiệm, học hỏi |
| **LAYER 4 – Emergent Systems** | Khai thác, chế tạo, công nghệ, kinh tế, xã hội, văn hóa, tổ chức, tiến hóa sinh học |
| **LAYER 5 – Simulation, History & Verification** | Mô phỏng, lịch sử nhân quả, kiểm tra nhất quán, deterministic replay, tối ưu hiệu năng, công cụ kiểm định |

Đủ để chốt bản thiết kế kiến trúc cấp cao — nhưng chưa đủ để tuyên bố engine hoàn thiện. Bước tiếp theo là **Master Architecture Specification**: định nghĩa invariants, trách nhiệm từng hệ thống, ranh giới hardcode/data/knowledge, luồng nhân quả xuyên suốt, giao diện module, tiêu chuẩn kiểm thử và lộ trình không phá vỡ codebase.

## **Triết lý phát biểu cuối cùng**

> **Thế giới tồn tại độc lập với chủ thể. Mọi thứ vận hành theo các quy luật tự nhiên. Chủ thể tồn tại trong thế giới, có nhu cầu và giới hạn riêng. Chúng nhận thức thế giới thông qua quan sát và trải nghiệm, hình thành tri thức, đặt mục tiêu, thử nghiệm hành động và tạo ra những biến đổi có nguyên nhân. Những biến đổi đó tiếp tục ảnh hưởng đến môi trường, sinh vật, cộng đồng và các thế hệ tiếp theo. Từ đó, thế giới tự hình thành lịch sử và tiến hóa.**

## **Nguyên tắc triển khai: đừng bắt đầu bằng nội dung**

Đừng bắt đầu bằng việc viết hàng trăm hệ thống công nghệ, tài nguyên hoặc hành vi NPC. Hãy bắt đầu bằng việc hoàn thiện **bộ máy nhân quả, tương tác, nhận thức và lập kế hoạch**. Nếu bốn bộ máy này đủ tốt, mới có nền tảng thêm nội dung mà không liên tục phải viết lại logic game.

Lưu ý giới hạn kỹ thuật: engine không tự hiểu mọi định luật vật lý hay ngành khoa học mới chỉ nhờ thêm một dòng data — mở rộng domain mới vẫn cần solver/mô hình tương ứng. Mục tiêu là các domain đã hỗ trợ kết hợp linh hoạt mà không hardcode từng tình huống.

## **Một quyết định rất quan trọng: Không xây dựng một AI toàn năng**

Tôi không muốn Sunnyworld có một module tên `UniversalAI` rồi module đó tự quyết định mọi thứ.

Tôi muốn mỗi hệ thống làm đúng một nhiệm vụ:

- Physics biết vật thể chuyển động thế nào.
- Chemistry biết vật chất biến đổi thế nào.
- Biology biết sinh vật sống và sinh sản thế nào.
- Causality biết tại sao sự kiện xảy ra.
- Agent Intelligence biết NPC có thể suy nghĩ gì.
- Discovery biết NPC có thể khám phá gì.
- Evolution biết quần thể và hệ thống thay đổi ra sao.

Những module này kết hợp với nhau để tạo ra sự phức tạp.

Đó là cách tiếp cận bền vững hơn rất nhiều so với cố gắng viết một bộ AI có thể làm tất cả.

# **PHẦN X – VIỆC NÊN LÀM NGAY BÂY GIỜ**

Tôi khuyên bạn chưa nên yêu cầu Claude Code lập tức viết toàn bộ engine mới.

Bản thiết kế cấp cao đã đủ để chốt. Việc tiếp theo nên là xây dựng **Master Architecture Specification** — một tài liệu định nghĩa: các quy luật bất biến; hệ thống và trách nhiệm; những gì được phép hardcode và những gì tuyệt đối không; data model và ranh giới World Truth ↔ NPC Knowledge; luồng nhân quả xuyên suốt; giao diện giữa các module; tiêu chuẩn kiểm thử Open-ended Evolution; lộ trình triển khai không phá vỡ codebase hiện tại.

Mục tiêu không phải làm ra một game có thật nhiều tính năng — mà là một engine có thể tạo ra những tính năng và hệ thống mà chính chúng ta chưa viết sẵn.

Sau đó mới yêu cầu agent kiểm tra dự án Sunnyworld hiện tại và xây dựng bản đồ kiến trúc đối chiếu với thiết kế mới.

Tôi đã soạn prompt để bạn có thể giao việc này.

# SUNNYWORLD – OPEN-ENDED EVOLUTION ENGINE

## Architecture Audit & Gap Analysis

Bạn đang làm việc trên dự án Sunnyworld.

Mục tiêu dài hạn của dự án là xây dựng một **Open-ended Evolution World Simulation Engine**, trong đó thế giới tiến hóa thông qua quy luật tự nhiên, tương tác vật lý, nhân quả, khám phá, thử nghiệm, di truyền và tiến hóa xã hội.

### 1. TRIẾT LÝ KIẾN TRÚC

Sunnyworld không được phát triển theo mô hình hardcoded technology tree hoặc scripted evolution.

Các nguyên tắc bắt buộc:

1. World Truth tồn tại độc lập với NPC.
2. Agent Belief không được đồng nhất với World Truth.
3. Mọi thay đổi thế giới phải có nguyên nhân và điều kiện.
4. NPC chỉ được sử dụng kiến thức mà chúng thực sự biết hoặc có thể tiếp cận.
5. Công nghệ phải được khám phá, thử nghiệm và xác minh.
6. Các cấu trúc mới có thể hình thành từ việc kết hợp những thực thể và vật liệu hiện có.
7. Sinh học phải hỗ trợ di truyền, biến dị, đột biến và chọn lọc tự nhiên.
8. Xã hội phải hình thành từ tương tác và nhu cầu thực tế.
9. Thêm dữ liệu mới không được buộc phải sửa toàn bộ engine.
10. Không được tạo ra kết quả giả chỉ để NPC đạt mục tiêu.

### 2. NHIỆM VỤ AUDIT

Đọc toàn bộ codebase và xác định trạng thái thực tế của từng module:

- World Core.
- Entity & Ontology.
- Property System.
- Material System.
- Natural Law System.
- Physics.
- Chemistry.
- Biology.
- Ecology.
- Interaction Resolver.
- Affordance Engine.
- Causal Engine.
- Composition Engine.
- Agent Intelligence.
- Discovery Engine.
- Experimentation Engine.
- Verification.
- Knowledge Transmission.
- Biological Evolution.
- Social Evolution.
- Cultural Evolution.
- World History.
- Simulation Scheduler.
- Deterministic Replay.
- Performance & LOD.

Với mỗi module, phân loại:

A. Đã có và hoạt động thực tế.

B. Đã có nhưng còn hardcoded hoặc chưa tổng quát.

C. Mới có một phần kiến trúc.

D. Chưa tồn tại.

Không đánh giá một module là hoàn thành chỉ vì đã có class hoặc file mang tên tương ứng.

Phải kiểm tra code thực tế, call graph, dependency và các bài kiểm tra.

### 3. KIỂM TRA HARD-CODE

Tìm kiếm:

- Danh sách công nghệ cố định.
- Danh sách sản phẩm cố định.
- Candidate Generator sử dụng điều kiện cho từng loại vấn đề.
- Công thức kết hợp vật liệu chỉ hỗ trợ các cặp được khai báo trước.
- Logic sinh vật chỉ hỗ trợ các loài được lập trình riêng.
- Logic xã hội chỉ tạo các tổ chức có sẵn.
- Các điều kiện tạo kết quả không dựa trên Natural Laws.
- Các đoạn code cấp phát tri thức miễn phí cho NPC.
- Các cơ chế tạo kết quả mà không có chi phí tài nguyên hoặc nhân quả.

Phân biệt hardcode cần thiết trong domain solver với hardcode làm giới hạn khả năng tiến hóa của toàn hệ thống.

### 4. KIỂM TRA KHẢ NĂNG MỞ RỘNG

Thiết kế và đánh giá những bài kiểm tra sau:

- Thêm một vật liệu mới mà không sửa engine.
- Thêm một thuộc tính mới được các solver hỗ trợ.
- Thêm một quy luật mới trong domain hiện có.
- Tạo một cấu trúc vật liệu chưa từng tồn tại trong dữ liệu ban đầu.
- NPC khám phá một phương pháp chưa có trong danh sách công nghệ.
- NPC thất bại khi thử nghiệm không đáp ứng điều kiện.
- Hai cộng đồng độc lập khám phá cùng một phương pháp.
- Thêm một loài sinh vật có đặc tính di truyền mới.
- Kiểm tra chọn lọc tự nhiên qua nhiều thế hệ.

Không giả lập kết quả thành công bằng cách thêm riêng một if/else cho test.

### 5. DEPENDENCY AUDIT

Vẽ dependency graph hiện tại.

Xác định:

- Module nào phụ thuộc quá nhiều vào module khác.
- Module nào có logic bị trùng lặp.
- Các vòng phụ thuộc.
- Những module đang chứa logic thuộc sai tầng.
- Những interface cần tách biệt.

### 6. KẾT QUẢ BẮT BUỘC

Tạo các tài liệu:

1. CURRENT_ARCHITECTURE.md
2. MODULE_CAPABILITY_MATRIX.md
3. HARDCODE_AUDIT.md
4. DEPENDENCY_GRAPH.md
5. OPEN_ENDED_GAP_ANALYSIS.md
6. MIGRATION_ROADMAP.md
7. ARCHITECTURE_RISK_REPORT.md

Trong MIGRATION_ROADMAP.md phải chia công việc thành từng phase, có dependency, acceptance criteria và test cụ thể.

### 7. QUY TẮC THỰC HIỆN

- Chỉ audit, chưa tự ý viết lại engine.
- Không xóa hoặc thay đổi hệ thống đang hoạt động.
- Không giả định một module hoạt động chỉ từ tên class.
- Không tạo thêm abstraction không có nhu cầu thực tế.
- Ưu tiên tái sử dụng những thành phần hiện có.
- Mọi nhận định phải có bằng chứng từ source code.
- Phân biệt rõ khả năng đã chứng minh bằng test và khả năng mới chỉ được thiết kế.

### MỤC TIÊU CUỐI CÙNG

Đưa ra câu trả lời chính xác:

Sunnyworld hiện đang ở mức nào trên hành trình xây dựng Open-ended Evolution Engine?

Những thành phần nào có thể giữ lại?

Những thành phần nào phải tổng quát hóa?

Những thành phần nào cần xây dựng mới?

Và lộ trình ít rủi ro nhất để chuyển dự án hiện tại thành một Causal World Simulation Engine thực sự.

## **Kết luận của tôi**

Tôi muốn bạn giữ một hình dung xuyên suốt khi phát triển Sunnyworld:

**Bạn không đang xây dựng một trò chơi có hàng nghìn công nghệ, hàng nghìn vật phẩm và hàng nghìn kịch bản. Bạn đang xây dựng một hệ thống có khả năng tạo ra lịch sử từ những quy luật nền tảng.**

Nếu kiến trúc thành công:

- Một vật liệu mới có thể mở ra những phương pháp chế tạo mới.
- Một sinh vật mới có thể thay đổi hệ sinh thái.
- Một phát minh có thể thay đổi kinh tế và chiến tranh.
- Một thảm họa có thể làm biến đổi tín ngưỡng và tổ chức xã hội.
- Một nền văn minh có thể phát triển theo con đường hoàn toàn khác nền văn minh khác.

Và quan trọng nhất: bạn không cần ngồi viết trước lịch sử đó.

**Tôi khuyên bước tiếp theo là hoàn thành Architecture Audit trước, sau đó ưu tiên xây dựng ba nền tảng: Natural Law System, Causal Engine và Universal Composition Engine.** Đây là ba nền móng quyết định Sunnyworld có thực sự thoát khỏi mô hình hardcode hay không.