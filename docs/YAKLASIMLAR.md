# SAkGD — Yaklaşımlar ve Metotlar Dokümantasyonu

Bu doküman repodaki tüm çözücü yaklaşımlarını, ortak altyapıyı ve **temel aldığımız
2025 kazanan makalesinin üzerine eklediklerimizi** açıklar.

> **Problem (GD 2025 Live Challenge):** Verilen bir grafın düğümlerini, belirlenen
> bir grid üzerinde tam sayı koordinatlara yerleştir; iki düğüm aynı noktada
> olamaz ve bir düğüm, ait olmadığı bir kenarın üzerinde duramaz. Amaç
> **k = herhangi bir tek kenarın üzerindeki maksimum kesişme sayısını** minimize
> etmek (eşitlikte toplam kesişme sayısı daha az olan kazanır).

---

## 1. Temel: SAkGD makalesi (önceki senenin kazananı)

Repo, yarışmayı kazanan yaklaşımın birebir yeniden uygulamasıyla başladı:
Bianchetti & Moalic, *"Winning the GD Challenge for the 4th Time"*, GD 2025
([LIPIcs.GD.2025.43](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.GD.2025.43)).

Makalenin üç adımlı tarifi:

1. **"Fena olmayan" bir başlangıç çözümü** — OGDF kütüphanesiyle Stress
   Minimization (1 kez, deterministik) + FMMM'i ilk 1 dakika boyunca tekrar
   tekrar çalıştırıp en düşük k'lı adayı seçmek.
2. **Faz 1 — toplam kesişme SA'sı:** fitness = ΔC (toplam kesişme değişimi);
   kötüleşen hamle `exp(−ΔC/T)` olasılığıyla kabul. Parametreler: T₀=50,
   decT=0.999, decTW=0.99 (dalga başına), tLim=0.01.
3. **Faz 2 — k-değeri SA'sı:** birincil fitness = hamleye karışan kenarlardaki
   *yerel* maksimum kesişme değişimi; eşitlikte toplam kesişme (normalize).
   Parametreler: T₀=1, decT=0.9999, decTW=0.99, tLim=0.01.

Ortak iskelet (Algorithm 1): **dalga (wave) yapısı** — sıcaklık her dalgada
`decTW` ile kırpılarak resetlenir ve her dalga, bilinen en iyi çözümden yeniden
başlar. `selectNode()` kesişmesi yoğun kenarlara dokunan düğümleri,
`selectPlace()` mevcut konuma yakın pozisyonları (Gauss, σ ∝ T/T₀; Faz 1'de %5
global sıçrama) tercih eder.

Bizim `./sakgd` (`src/main.cpp`) bu iki fazı **aynı parametrelerle** uygular.
Makalenin vermediği her şey (veri yapıları, init, paralellik) bizim katkımızdır
— aşağıda §4'te listelenmiştir.

---

## 2. Ortak altyapı (tüm metotların paylaştığı çekirdek)

| Bileşen | Açıklama |
|---|---|
| **Artımsal kesişme sayımı** (`MovePlan`, `planMove`/`commitMove`) | Bir düğüm hamlesinin etkisi global yeniden sayım yapılmadan hesaplanır: sadece düğüme komşu kenarlar ve onların kesişme ortakları güncellenir. `xs[e]` (kenar başına kesişme kümesi), `xc[e]` (sayı), `cntPerK[k]` (k seviyesindeki kenar sayısı) ve `kVal` artımsal tutulur. |
| **Uzamsal grid indeksi** | Kenarlar, bounding-box'larının kapsadığı hücrelere kaydedilir (`gridSide = √m/1.5`, 8–256 arası). Kesişme testi yalnızca aynı hücreleri paylaşan adaylara yapılır. |
| **Geçerlilik koruması** | `occupied` haritası (iki düğüm aynı noktada olamaz) + `wouldCauseVertexEdgeOverlapFast` (düğüm, ait olmadığı kenarın üzerine inemez). Geçersiz başlangıç layoutları setup'ta onarılır. |
| **Başlangıç stratejileri** (`--init auto|input|bfs`) | `auto`: girdi layoutu ile BFS-snake'in örneklenmiş kesişme yoğunluğunu karşılaştırır, belirgin biçimde seyrek olanı (%20 eşik) seçer. `bfs`: BFS sırasıyla boustrophedon (yılan) yerleşim + hücre içi jitter, ardından barycenter düzeltmesi (50 tur komşu ortalamasına çekme). |
| **Çıktı/izleme** | `--status-file` canlı JSON durumu, `--trace-file` yakınsama izi, `--verify` çözüm doğrulama. |

---

## 3. Metotlar (`run_contest.py` metot kayıt tablosu)

Bir *metot*, sıralı *stage*'lerden oluşur; çok aşamalı metotlar warm-start ile
zincirlenir (sonraki stage öncekinin çıktısını girdi alır).

### 3.1 `sa` — Saf iki fazlı SA
Makalenin Algorithm 1'i, yukarıdaki parametrelerle. Tek düğüm hamleleri.
Başlangıç: `--init auto` (girdi vs BFS-snake).

### 3.2 `sa-stress` — Stress-init + SA ⭐ (en güçlü metot)
İki stage:
1. **`tools/stress_init.py`** (bütçenin %8'i): graphviz **sfdp** (çok seviyeli
   kuvvet yönlü, OGDF FMMM muadili) ve küçük graflarda ek olarak **neato**
   (stress majorization, OGDF SM muadili) ile en çok 8 deneme yapar; her deneme
   rastgele döndürülüp grid'e ölçeklenir, tam sayıya snap'lenir (çakışmalar
   spiral aramayla en yakın boş hücreye itilir) ve **örneklenmiş kesişme
   yoğunluğu** (numpy ile ~200k rastgele kenar çifti) ile puanlanır; en iyi aday
   yazılır. n>3000'de yalnız sfdp (hem hızlı hem deneylerde hep daha iyi).
2. **`sakgd`** (kalan %92): stress layoutundan warm-start. Init modu `auto`
   bırakılır — init stage'i başarısız olursa BFS-snake güvenlik ağı devreye girer.

Bu, kazanan makalenin 1. adımının (SM + tekrarlı FMMM + en iyi adayı seç)
OGDF'siz muadilidir; OGDF'nin Python bağları macOS'ta çalışmadığı için graphviz
CLI kullanıldı.

### 3.3 `lns` — Large Neighbourhood Search (`approach1 --mode lns`)
Tek düğüm yerine **bağlantılı bir düğüm grubu** taşınır:
- **Destroy:** kesişme ağırlıklı seçilen tohum düğümden BFS ile K düğümlük
  komşuluk büyütülür (K varsayılan n/10, dev graflarda 64 ile sınırlı).
- **Repair:** gruptaki her düğüm için R rastgele aday pozisyon denenir
  (`--nh-cands`, varsayılan 50), **yalnızca kesin iyileştiren** en iyi hamle
  commit edilir.
- **Restart:** her `500/K` iterasyonda en iyi layouta dönülür.

Not: yalnız iyileştiren hamleleri kabul ettiği için gerçek bir
destroy-and-repair LNS'i değildir; yerel optimumda durur (bkz. hafıza notu
"LNS-is-not-true-LNS").

### 3.4 `lns-adaptive`
LNS ile aynı; her 50 iterasyonda iyileşme oranına göre komşuluk boyutunu
ayarlar: oran < %5 ise K×1.5 (genişle), > %25 ise K÷1.3 (daralt).

### 3.5 `ils` — Iterated Local Search (`approach1 --mode ils`)
LNS'in takılma problemine cevap: tam iki fazlı SA koşuları ile rastgele
**kick**'ler dönüşümlü çalışır. İç SA bütçesi `toplam/5`; kick, global en iyiyi
geri yükleyip P düğümü (`--ils-perturb`, varsayılan n/10) rastgele konumlara
savurur. Global en iyi korunur.

### 3.6 `staged` / `staged-adaptive`
İki stage: LNS (veya LNS-adaptive) bütçenin %30'u → çıktısından warm-start'lı
SA %70. LNS'in kaba küresel düzenlemesi + SA'nın ince ayarı.

### 3.7 `*-base` varyantları (`sa-base`, `ils-base`, …)
Aynı algoritmalar, Faz 2'de **k-kritik düğüm seçimi kapalı** (`--kband -1`).
Yeni seçim mekanizmasının A/B testi için tutulur.

### Deneysel bayraklar (varsayılan KAPALI — ablation'da kaybettiler)
- **`--lexk 1`:** Faz 2 fitness'ına orta terim olarak "k seviyesindeki kenar
  sayısı" eklenir (bir tepe kenarını eritmek 1/cntPerK[k] değerinde). Seyrek
  graflarda nötr, **yoğun A6'da belirgin gerileme** (632→735) → kapalı.
- **`--krepair 1`:** Dalgalar arasında deterministik cila — k seviyesindeki
  kenarların uç düğümleri için örneklenmiş aday kümesi tam değerlendirilir,
  kesin iyileştiren en iyi hamle commit edilir (Radermacher et al., JEA 2019
  "vertex movement" ilkesinden). **A8'de zararlı** (7→11; soğumuş SA dengesini
  bozuyor) → kapalı.

---

## 4. Önceki senenin (makalenin) üzerine neler koyduk?

Etki sırasına göre:

1. **Stress/force-directed init — `sa-stress`** *(en büyük kazanç)*.
   Makale OGDF kullanır ama repo başlangıçta BFS-snake ile çalışıyordu; sfdp
   tabanlı init eklenince seyrek-büyük graflarda k 3-4 kat düştü (aşağıdaki
   tabloya bakın). "Spring embedder'lar kraldır" — GD'25 otomatik kategorisinin
   ilk üç takımı da kuvvet yönlü init kullandı.
2. **k-kritik düğüm seçimi (`--kband`, varsayılan 2):** Faz 2'de seçim
   ağırlıkları, kesişme sayısı `k−kBand`'den yüksek kenarlara dokunan düğümlere
   kaydırılır. Makaledeki "yoğun kenarlı düğümleri seç" fikrinin, darboğaz
   kenarlarına odaklanan keskinleştirilmiş hali. `*-base` varyantlarıyla A/B
   test edilebilir.
3. **BFS-snake + barycenter başlangıcı ve `--init auto` seçici:** girdi
   layoutu ile yapıcı layout örneklenmiş yoğunlukla karşılaştırılır. Stress-init
   başarısız olduğunda güvenlik ağı olarak hâlâ kritik.
4. **Alternatif metaheuristikler:** LNS, LNS-adaptive, ILS ve staged
   pipeline'lar (makalede yalnız SA vardır). ILS bazı orta boy graflarda
   (A7/A9) saf SA'yı geçmişti; stress-init sonrası ana değerini kaybetti ama
   portföyde duruyor.
5. **Paralel multi-start orkestrasyonu (`run_contest.py`):** graf × metot
   kombinasyonu başına N worker (farklı tohumlar), grafa göre bütçe grupları
   (küçük 5 dk / orta 8 dk / büyük 15 dk), en iyi worker'ın seçimi,
   `results/bests.json` kalıcı rekor takibi, `results/report.html` etkileşimli
   rapor + canlı durum, `dashboard.py` canlı izleme.
6. **Mühendislik hızlandırmaları:** artımsal `MovePlan` delta hesabı, uzamsal
   grid, koşullu dalga geri yüklemesi (en iyi çözüme %1 bandında ise pahalı
   restore atlanır — A8 Faz 1'de 32 sn kazanç), hızlı vertex-edge overlap
   testi, geçersiz girdi onarımı.
7. **Negatif sonuçlarıyla belgelenmiş deneyler:** `--lexk` ve `--krepair`
   (yukarıda) — ikisi de literatür destekli fikirlerdi, ikisi de kısa A/B
   testlerinde elendi ve bayrak arkasında tutuluyor.

---

## 5. Güncel sonuçlar (2026-06-10)

8 worker, bütçeler: A6/A7/A9 = 8 dk (A6 finali 15 dk), A8 = 15 dk.

| Graf | n / m | Eski en iyimiz | **Yeni en iyimiz (metot)** | Rakip | GD'25 kazananı |
|---|---|---|---|---|---|
| Automatic-6 | 200 / 3000 | 632 | **624** (sa, 15 dk) | 621 | 568 |
| Automatic-7 | 500 / 1740 | 39 | **28** (sa-stress) | 31 | 31 |
| Automatic-8 | 10466 / 20288 | 31 | **7** (sa-stress) | 12 | 15 |
| Automatic-9 | 2519 / 4938 | 21 | **10** (sa-stress) | 13 | 12 |

Okumalar:
- **Seyrek graflar (A7/A8/A9, m/n ≈ 2-3.5):** kazanç tamamen init kalitesinden.
  A8 ve A9 neredeyse planar yoğunlukta; iyi bir küresel yerleşim k'yı baştan
  düşürüyor, SA yalnızca cilalıyor.
- **Yoğun graf (A6, m/n = 15):** stress init işe yaramıyor (sa-stress 765 >
  sa 735); tek etkili kaldıraç bütçe (8 dk: 632 → 15 dk: 624). Kazananın 568'i
  49 dakikalık koşudandı — A6'da sıradaki adım 30-60 dakikalık koşu.

---

## 6. Hızlı kullanım

```bash
make                                   # ./sakgd ve ./approach1

# Tek graf, stress-init + SA (önerilen, seyrek graflar)
python3 tools/stress_init.py -i data/.../Automatic-8.json -o /tmp/a8_init.json -s 1 -t 60
./sakgd -i /tmp/a8_init.json -o out.json -t 15 -p1 4 --init input

# Tam pipeline (metot karşılaştırmalı, 8 worker)
python3 run_contest.py --methods sa-stress,sa --graphs 6-9 --workers 8

# Doğrulama
./sakgd --verify out.json -i out.json
```
