# Vendor SDK Directory

Bu klasör vendor SDK'larını içerir. **Lisans nedeniyle bu dosyalar Git'e eklenmez.**

## 📦 Kurulum

### Alif Ensemble SDK

```bash
# Otomatik indirme (önerilen)
./scripts/download_vendor_alif.sh

# Manuel kurulum
# 1. Alif Ensemble SDK'yı indirin:
#    https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases
# 2. vendor/alif/ klasörüne çıkarın
```

### Renesas FSP (Gelecek)

```bash
# Otomatik indirme
./scripts/download_vendor_renesas.sh

# Manuel kurulum
# 1. Renesas FSP'yi indirin:
#    https://github.com/renesas/fsp/releases
# 2. vendor/renesas/ klasörüne çıkarın
```

### STM32 HAL (Gelecek)

```bash
# Otomatik indirme
./scripts/download_vendor_stm32.sh
```

---

## 🔒 Lisans Notları

Vendor SDK'lar kendi lisanslarına tabidir:
- **Alif Ensemble:** [Alif License Agreement](https://alifsemi.com/license)
- **Renesas FSP:** [FSP License](https://github.com/renesas/fsp/blob/master/LICENSE.md)
- **STM32 HAL:** [STM32 License](https://www.st.com/content/st_com/en/about/legal/legal-information.html)

Bu nedenle vendor SDK'lar bu repository'de saklanmaz.

---

## 🚀 Mock Driver ile Çalışma

**Vendor SDK olmadan çalışmak için Mock driver kullanabilirsiniz:**

```bash
cmake -B build -DBUILD_MOCK=ON
cmake --build build
./build/bin/getting_started
```

Mock driver ile tüm API'yi test edebilir ve öğrenebilirsiniz!

---

## 📂 Klasör Yapısı

```
vendor/
├── README.md              # Bu dosya
├── .gitkeep               # Klasörü git'te tut
│
├── alif/                  # Alif Ensemble SDK (git'e eklenmez)
│   ├── cmsis-driver/
│   └── ...
│
├── renesas/               # Renesas FSP (git'e eklenmez)
│   ├── ra/
│   └── ...
│
└── stm32/                 # STM32 HAL (git'e eklenmez)
    ├── Drivers/
    └── ...
```

---

## ❓ Sorun Giderme

### "ALIF_SDK_PATH is required" hatası

```bash
# Çözüm 1: Otomatik indirme scriptini çalıştırın
./scripts/download_vendor_alif.sh

# Çözüm 2: CMake'e path verin
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=/path/to/alif/sdk
```

### "vendor/alif/ does not exist" hatası

```bash
# vendor klasörünün içinde olduğunuzdan emin olun
pwd  # Çıktı: /path/to/alp-sdk olmalı

# Alif SDK'yı indirin
./scripts/download_vendor_alif.sh
```

---

## 💡 İpuçları

1. **İlk kez kullanıyorsanız:** Mock driver ile başlayın
2. **For real hardware:** Download the relevant vendor SDK
3. **For CI/CD:** Use Mock driver (no vendor SDK required)

---

<p align="center">
  <strong>Need help?</strong> <a href="https://github.com/alplabai/alp-sdk/issues">Open an issue</a>
</p>
