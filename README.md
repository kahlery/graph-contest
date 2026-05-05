# SAkGD — 2025 Graph Drawing Contest k-planarity Çözücüsü (C++)

Bu proje, Bianchetti & Moalic'in 2025 GD Contest'ini kazanan **SAkGD** yaklaşımının
([LIPIcs.GD.2025.43](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.GD.2025.43))
sıfırdan yazılmış, harici bağımlılığı olmayan bir C++ uyarlamasıdır.

> Verilen bir grafın düz çizgili çiziminde, bir kenarın kesiştiği başka kenar sayısının
> maksimumuna **k-değer** (k-planarlık parametresi) denir. Amaç bu k-değerini
> minimize etmektir.

## Yaklaşım (paper'ı birebir izler)

Üç aşamalı bir meta-sezgisel:

1. **Aşama 1 — Başlangıç çizimi**
   Girdi JSON dosyasındaki konumlar başlangıç çözümü olarak alınır. (Paper, OGDF'in
   Stress-Minimization ve FMMM algoritmalarını da deniyor; bu uyarlama bağımsızlık
   için OGDF'siz çalışır, fakat girdiyi olduğu gibi başlangıç olarak kullanır.)

2. **Aşama 2 — Toplam kesişimi azaltan SA** (varsayılan 10 dk)
   Algoritma 1'in iskeletinde, fitness olarak **toplam kesişim sayısındaki değişim**
   `ΔC` kullanılır. Kötüleşen hareket `exp(-ΔC / T)` olasılığıyla kabul edilir.
   Parametreler (Tablo 1): `T0=50`, `decT=0.999`, `decTW=0.99`, `tLim=0.01`.

3. **Aşama 3 — k-değerini optimize eden SA** (kalan süre)
   Aynı SA iskeletinde, fitness olarak iki katmanlı bir kriter kullanılır:
   - **Birincil**: hareketle "etkilenen" kenarlardaki yerel max kesişim sayısının değişimi
     (taşınan düğümün komşu kenarları + onları kesen kenarlar).
   - **İkincil (kopukluk durumunda)**: toplam kesişim sayısındaki değişim.
   Parametreler: `T0=1`, `decT=0.9999`, `decTW=0.99`, `tLim=0.01`.

`selectNode()` ve `selectPlace()` paper'ın tarif ettiği gibi:

- **selectNode**: çok kesişen kenarlara sahip düğümlere yüksek olasılık verilir
  (ağırlık ≈ `1 + Σ kesişim_sayısı(e)`). Düğüm seçimi kümülatif ağırlık dizisi
  üzerinde ikili arama ile yapılır.
- **selectPlace**: yeni konum, mevcut konumun etrafında bir Gauss dağılımından
  örneklenir (sıcaklığa bağlı σ). Yerel optimumdan kaçmak için %5 olasılıkla
  uniform global örnekleme yapılır (Aşama 1'de).

### Çözüm geçerliliği (yarışma kuralları)

Bu çözücü iki ayrık geometrik kısıtı **her hareket öncesi** zorlar — geçersiz
bir çözümün hiçbir zaman kabul edilmemesini garanti eder:

1. **Vertex tekilliği**: hiçbir iki düğüm aynı tam-sayı koordinatında olamaz.
2. **Vertex-edge overlap yasağı**: hiçbir düğüm, ait olmadığı bir kenarın iç
   noktasında olamaz (kollineer ve segment içinde). Aksi halde o kenarın
   "kesişim sayısı" belirsizleşir ve yarışma doğrulayıcısı çözümü reddeder.

Her ikisi de:
- `setup()` aşamasında: girdi geçersizse küçük perturbasyonlarla çözülür.
- `runSA()` döngüsünde: ihlali olan hareket düşük-maliyetle reddedilir
  (planlanmaz bile).
- Çıkış öncesi: en iyi çözümün geçerli olduğu son bir kez doğrulanır
  (`vertexEdgeOverlap=no`).

`./sakgd --verify dosya.json` çıktısında bu kontrolün sonucu da basılır
(geçerli ise `no`, geçersiz ise `yes` ve exit code 2).

### Performans optimizasyonları

- **Spatial grid**: tüm `O(m²)` kesişim taramasından kaçınmak için, kenarlar
  bounding-box'larına göre hücrelere kaydedilir. Bir kenar taşındığında yalnızca
  ilgili hücrelerdeki adaylar test edilir.
- **İnkremental kesişim takibi**: her kenarın kesişen kenar kümesi `xs[e]`,
  kesişim sayacı `xc[e]`, toplam kesişim `totalX`, ve `cntPerK[k]` üzerinden
  global k güncellenir; tam yeniden hesap yalnızca her dalga sonunda yapılır.
- **Plan→commit modeli**: önce hareket simüle edilir (state'e dokunulmaz), kabul
  edilirse uygulanır. Reddedilen hareketin geri alınması bedava (commit yapılmaz).

## Derleme

```bash
make           # release (-O3 -DNDEBUG)
make debug     # debug + ASAN/UBSAN
```

Standart bir GCC veya Clang yeterlidir; harici kütüphane yok.

## Kullanım

```bash
# Yarışma standardı: toplam 60 dk, ilk 10 dk Aşama 1
./sakgd -i input.json -o output.json -t 60 -p1 10

# Pozisyonel kısa yol (varsayılanlar 60 / 10):
./sakgd input.json output.json

# Sadece doğrulama (kesişim ve k raporlar):
./sakgd --verify output.json

# Tekrarlanabilir koşu için seed:
./sakgd -i input.json -o output.json -s 12345
```

### Girdi/çıktı JSON formatı

```json
{
  "width": 1000000,
  "height": 1000000,
  "nodes": [
    { "id": 0, "x": 100, "y": 200 },
    ...
  ],
  "edges": [
    { "source": 0, "target": 1 },
    ...
  ]
}
```

`id` alanı string veya sayı olabilir. `x` ve `y` her düğüm için verildiğinde
başlangıç düzenlemesi olarak kullanılır; eksikse rastgele atanır. Çıktı dosyası
girdi yapısını koruyup yalnızca `nodes[*].x` ve `nodes[*].y`'yi günceller.

## Doğrulama (test örnekleri)

`data/k5.json` ve `data/k6.json` küçük tam graflar:

| Graf | Bilinen optimum k | Bilinen optimum toplam kesişim | Bu kod |
|------|--------------------|--------------------------------|--------|
| K5   | 1                  | 1                              | k=1, X=1 ✓ |
| K6   | 1                  | 3                              | k=1, X=3 ✓ |

```bash
./sakgd -i data/k5.json -o data/k5.out.json -t 0.2 -p1 0.05
./sakgd --verify data/k5.out.json
```

## Canlı Web Arayüzü (dashboard)

Birden fazla SA denemesini paralel çalıştırıp her birinin **hangi aşamada
olduğunu** ve mevcut en iyi sonucu canlı izleyebileceğiniz bir tarayıcı
arayüzü.

### İnteraktif mod (en kolayı)

Argümansız çalıştırınca tüm parametreler tek tek sorulur:

```bash
./dashboard.py
```

Her soruda Enter'a basarak varsayılan değeri kabul edebilirsin. Akıllı
varsayılanlar:

- `data/` dizinindeki tüm `.json` graflar listelenir; numara seç veya
  elle yol gir.
- Toplam ve Aşama 1 süreleri için makul varsayılanlar.
- HTTP portu: 8765 dolu ise otomatik olarak boş bir port önerilir.
- Tarayıcı otomatik açma seçeneği (`e` / `h`).

İnteraktif moda zorlamak için (CLI argümanlarıyla birlikte) `-I` /
`--interactive` da kullanılabilir.

### CLI modu

```bash
./dashboard.py data/random30.json -n 4 -t 2 -p1 0.4
```

Argümanlar:

| Bayrak | Anlamı | Varsayılan |
|--------|--------|-----------|
| `-n / --workers` | paralel SA çalışan sayısı | 4 |
| `-t / --minutes` | her bir çalışan için toplam süre (dk) | 60 |
| `-p1 / --phase1-minutes` | her bir çalışanın Aşama 1 süresi (dk) | 10 |
| `-p / --port` | HTTP sunucu portu | 8765 |
| `--seed` | taban seed (çalışan N → seed+N kullanır) | 42 |
| `--out-dir` | çıktı / status dosyaları dizini | `runs/` |
| `--status-interval` | status JSON yazma aralığı (sn) | 1.0 |
| `--no-open` | tarayıcıyı otomatik açma | |

Arayüzde her çalışan için bir kart vardır:

- **Faz rozeti**: `Aşama 1: kesişim azaltma` (mavi) ↔ `Aşama 2: k optimize`
  (mor) ↔ `Bitti` (yeşil).
- **Canlı / Bitti rozeti**: yanıp sönen yeşil = çalışıyor, kırmızı = exit.
- **En iyi rozeti**: o anda küresel olarak en iyi sonucu üreten çalışan
  sarı `EN İYİ` etiketi alır.
- **Sayısal istatistikler**: `k`, `en iyi k`, toplam kesişim, en iyi toplam,
  hareket sayısı, kabul oranı, mevcut sıcaklık, sıcaklık limiti.
- **İlerleme barları**: faz süre kullanımı + sıcaklığın limit ile log
  ölçekteki konumu.
- **Canlı SVG**: en iyi çizimin küçük bir önizlemesi (her saniye yenilenir).

Üst kısımdaki özet panelinde aktif çalışan sayısı, küresel en iyi `k` ve
toplam kesişim, ve kümülatif hareket / kabul oranı görüntülenir.

> Solver tarafında bu özelliği aktive eden iki yeni bayrak vardır:
> `--status-file PATH` (JSON status dosyası yolu) ve `--status-id STRING`
> (kart başlığında görüntülenecek isim). `dashboard.py` bunları otomatik
> ayarlar; tek bir koşu için manuel kullanım da mümkündür.

## Yarışma için ipuçları

- **Süre yönetimi**: paper'da toplam 60 dk, ilk 10 dk Aşama 1. Çok büyük graflarda
  Aşama 1'i 15–20 dk yapmayı deneyin.
- **Çoklu seed**: SA stokastiktir. Aynı süreyi birden fazla seed ile koşturup en
  iyiyi alma stratejisi her zaman işe yarar. En kolay yolu yukarıdaki
  `dashboard.py`'tir; kabuk üzerinden de yapılabilir:
  ```bash
  for s in 1 2 3 4 5 6 7 8; do
    ./sakgd -i graph.json -o out_$s.json -t 12 -p1 2 -s $s &
  done; wait
  for s in 1 2 3 4 5 6 7 8; do ./sakgd --verify out_$s.json; done
  ```
- **OGDF ile başlangıç**: OGDF kuruluysa, Stress-Minimization veya FMMM çıktısını
  ayrı bir araçla `nodes[*].x/y` alanlarına yazıp ardından bu çözücüye verin.
  Kod, JSON'daki konumları başlangıç olarak alır.

## Kaynak

- Bianchetti, J., & Moalic, L. (2025). *Winning the GD Challenge for the 4th Time:
  Our Approach.* GD 2025 (LIPIcs.GD.2025.43).

## Dosya yapısı

```
GraphContest/
├── Makefile             # make / make debug / make clean
├── README.md
├── dashboard.py         # paralel koşum + HTTP server (canlı arayüz)
├── dashboard/
│   └── index.html       # vanilla JS dashboard
├── src/
│   └── main.cpp         # tüm solver (JSON parser dahil, harici bağımlılıksız)
├── data/                # girdi grafları + üretilen *.out.json çıktılar
│   ├── k5.json          # küçük doğrulama örnekleri
│   ├── k6.json
│   └── random30.json    # arayüz testi için 30 düğümlü rastgele graf
└── runs/                # dashboard çalıştığında her seed için
                         # status.json + output.json + log.txt
```
