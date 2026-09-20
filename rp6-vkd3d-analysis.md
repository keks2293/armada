# Анализ: vkd3d feature level на Retroid Pocket 6 (Armada OS)

**Вопрос:** почему игры в DX12 на RP6 с Armada OS не доходят до D3D12 12_0/12_1
(уровень «меньше 12»), хотя по коду «должно доходить».

**Дата:** 19.09.2026
**Изображение:** Armada main (Mesa 26.2.3 + 3 патча, kernel 7.2.6, CachyOS Proton 11)

---

## 1. Устройство

| Компонент | Значение | Источник |
|---|---|---|
| SoC | Qualcomm Dragonwing QCS8550 (IoT-вариант SM8550) | LineageOS wiki, `retroid-pocket-6.conf` (`ARMADA_SOC_CLASS=SM8550`) |
| GPU | **Adreno A740** (не A750!) | `qcs8550-ayn-common.dtsi:957-962` (`zap-shader: qcom/sm8550/a740_zap.mbn`), спецификация Retroid «Adreno 740» |
| Панель | Visionox VTDR6130 AMOLED, 5.5" 1080p@120Hz | `qcs8550-retroidpocket-rp6.dts` (`compatible = "vtdr6130,rp6"`), Retroid |
| Ядро Armada | ванильный kernel.org 7.2.6 + 148 патчей + DTS + config-оверрайды | `armada-os/armada-packages/kernel/` (BASE.env, build-kernel.sh) |
| Mesa/Turnip | 26.2.3 + 3 патча Armada | `armada-os/armada-packages/mesa/` (pin по sha256 в Containerfile) |
| Proton | CachyOS Proton 11 (arm64), vkd3d в составе | `build_files/30-install-steam-session.sh:105-133` |

---

## 2. Как vkd3d выбирает feature level

vkd3d-proton, `libs/vkd3d/device.c` (уровни — это `D3D_FEATURE_LEVEL`, где
12_0/12_1/12_2 = D3D12 1.0/1.1/1.2):

### 2.1 Вычисление `caps->max_feature_level` (`d3d12_device_caps_init_feature_level`, device.c:9585-9621)

Цепочка гейтов по Vulkan-возможностям драйвера:

```
11_0 (старт)
  → 11_1:  logicOp && vertexPipelineStoresAndAtomics &&
           maxPerStageDescriptor{StorageBuffers,StorageImages} >= 32
  → 12_0:  TiledResourcesTier >= 2   ← нужен SPARSE RESIDENCY из драйвера
           && ResourceBindingTier >= 2 (в vkd3d хардкод 3)
           && TypedUAVLoadAdditionalFormats
  → 12_1:  ROVsSupported && ConservativeRasterizationTier >= 1
           где ROVsSupported = fragmentShaderPixelInterlock
                             && fragmentShaderSampleInterlock   (device.c:9211-9212)
  → 12_2:  «DX Ultimate»: SM 6.5, WaveOps, Int64 shader ops,
           MeshShaderTier>=1, VRS Tier 2, RaytracingTier>=1.1,
           SamplerFeedback>=0.9, ResourceBindingTier>=3, TiledResourcesTier>=3, ...
```

### 2.2 Оверрайт `VKD3D_FEATURE_LEVEL` (device.c:9895-9964)

- Допустимые значения: `11_0`, `11_1`, `12_0`, `12_1`, `12_2`
- **Принудительно** ставит `caps->max_feature_level = <значение>` и поднимает
  соответствующие опции (device.c:9929-9949), не проверяя реальные возможности
  драйвера; лог `WARN("Overriding feature level: ...")`
- (Имя переменной именно `VKD3D_FEATURE_LEVEL`, не `VKD3D_LEVEL`)

### 2.3 Что получает игра

- `D3D12CreateDevice` принимает только **минимальный** уровень; проверка
  `min <= max_feature_level`, иначе E_INVALIDARG (device.c:10279-10281)
- Устройство работает на **максимуме vkd3d**, а не на том, что просила игра
- Запрос `D3D12_FEATURE_FEATURE_LEVELS` возвращает наибольший из запрошенных
  уровней, `<= max_feature_level` (device.c:4651-4662)

**Вывод:** уровень нигде не «прописывается» — он вычисляется при создании
устройства по тому, что драйвер сообщил через Vulkan-запросы.

---

## 3. Что выставляет Turnip (Mesa main ≈ 26.x; в образе 26.2.3)

Turnip — open-source Vulkan-драйвер для Adreno внутри Mesa (единственный
Vulkan для Adreno на Linux; проприетарные драйверы Qualcomm — только Android).
В 26.x код: `src/freedreno/vulkan/` (ранее `src/gallium/drivers/turnip/`).

| Фича (гейт vkd3d) | Статус в Turnip | Где |
|---|---|---|
| `logicOp`, `vertexPipelineStoresAndAtomics` (11_1) | ✓ | `tu_device.cc:468,486` |
| `VK_EXT_conservative_rasterization` (12_1) | ✓ для A7xx (`chip >= 7`) | `tu_device.cc:336` |
| `filterMinmaxSingleComponentFormats` | ✓ | `tu_device.cc:1110` |
| sparse: `has_sparse = has_vm_bind` | **зависит от ядра** (VM_BIND) | `tu_knl_drm_msm.cc:1366-1367` |
| `has_sparse_prr` | = параметр ядра `MSM_PARAM_HAS_PRR` | `tu_knl_drm_msm.cc:1401` |
| `sparseResidencyStandard3DBlockShape` | ✗ false → tiled tier ≤ 2 (для 12_0 хватает) | `tu_device.cc:1345` |
| **`fragmentShaderPixelInterlock` / `fragmentShaderSampleInterlock`** | **не реализованы вообще** | отсутствует в `tu_device.cc` и списке расширений |

### Патчи Mesa в Armada (`armada-os/armada-packages/mesa/patches/`)

1. `0001-disable-turnip-sparse-sync` — убирает cross-sync gfx/sparse очередей;
   временный workaround **именно для A740** (translation-fault-краши) — т.е. для GPU RP6
2. `0002-add-a830-chip-id` — чип-ID Adreno 8xx (порт с ROCKNIX)
3. `0003-ir3-disable-bindless-ubo-const-lowering` — **SM8550-specific** фикс
   shader-компилятора (порт с ROCKNIX)

Ни один патч не понижает feature level.

### Итог по 12_0/12_1

- **12_0** достижим, если ядро дало sparse (см. раздел 4) — Turnip его просто
  отражает
- **12_1 недостижимо на любом Adreno с Turnip без оверрайта** — нет interlock
  (это gap upstream Mesa, не Armada и не QCS8550-специфика)
- **12_2** — и подавно (нет mesh shaders/VRS/RT tier 1.1/sampler feedback)

---

## 4. Ядро 7.2.6: VM_BIND / PRR — всё уже есть

Проверено по исходнику `linux-7.2.6.tar.xz` (cdn.kernel.org) — это ровно версия
из образа (PR #477 «Bump kernel to 7.2.6»):

| Звено | Статус | Где |
|---|---|---|
| UAPI: `MSM_PARAM_HAS_PRR=0x15` | ✓ | `include/uapi/drm/msm_drm.h:95` |
| UAPI: `MSM_PARAM_EN_VM_BIND=0x16` (opt-in, WO, once) | ✓ | `msm_drm.h:119` |
| Ioctl `MSM_VM_BIND` | ✓ | `drivers/gpu/drm/msm/msm_drv.c:802` |
| `HAS_PRR → adreno_smmu_has_prr()` | ✓ | `adreno/adreno_gpu.c:447-449` |
| `adreno_smmu_has_prr()` = устройство имеет `adreno_smmu_priv` с `set_prr_addr` | ✓ | `msm_gpu.h:295-303` |
| **Гейт PRR**: `compatible "qcom,smmu-500" && !"qcom,sm8250-smmu-500" && "qcom,adreno-smmu"` → ставит `set_prr_addr`/`set_prr_bit` | ✓ **проходит для RP6** | `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:393-401` |
| DTS: GPU провязан к SMMU (`iommus = <&adreno_smmu ...>`) | ✓ | `arch/arm64/boot/dts/qcom/sm8550.dtsi:2857-2858` |
| DTS: нод `adreno_smmu` включён (без status=disabled), compatible `"qcom,sm8550-smmu-500","qcom,adreno-smmu","qcom,smmu-500","arm,mmu-500"` | ✓ все 3 условия гейта | `sm8550.dtsi:3000-3042` |
| DTS: цепочка RP6 наследует sm8550 (`qcs8550.dtsi` = `#include "sm8550.dtsi"`, строка 6) | ✓ | `arch/arm64/boot/dts/qcom/qcs8550.dtsi` |
| Config: `CONFIG_ARM_SMMU=y` (defconfig), `ARM_SMMU_QCOM` def-y при ARCH_QCOM, `CONFIG_DRM_MSM=y`, `CONFIG_DRM_GPUVM=y` | ✓ | arm64 defconfig 7.2.6 + `armada-kernel.config.overrides` |

**Вывод:** патч для ядра добавлять **нечего** — ванильный 7.2.6 с текущими DTS
покрывает VM_BIND/PRR для QCS8550. ROCKNIX (источник поддержки устройств)
тоже строит ванильный 7.2 без каких-либо VM_BIND-патчей (их 7.2-серия: 4 патча,
ни один не про VM_BIND/SMMU).

---

## 5. Итоговая диагностика

Для RP6 на текущем main:

```
ожидается:  11_1 → 12_0 (если ядро дало sparse) → потолок 12_0 (нет interlock в Turnip)
факт у вас: < 12_0 (11_1)
```

Варианты причины 11_1:

1. **Старый образ** — kernel 7.2.6 и Mesa 26.2.3 пришли на этой неделе (PR #477, #475);
   перепрошиться на свежий билд
2. **Сбой рантайма** — SMMU не пророадился (часы/питание), или opt-in VM_BIND
   не прошёл → Turnip без sparse → гейт 12_0 не проходит
3. **Не то чтение** — DX11-игра (через DXVK максимум 11_1 — это норма), или игра
   сама запросила низкий уровень

### Команды на устройстве

```sh
vulkaninfo | grep -E "sparseBinding|sparseResidencyBuffer"   # sparse из Turnip
dmesg | grep -iE "smmu|adreno"                                # пророадился ли SMMU
VKD3D_DEBUG=info  # в игре → строка "Max feature level: 0x..."
```

- `sparseBinding: true` + уровень 12_0 → ожидаемо, дальше только оверрайт
- `sparseBinding: false` → смотреть dmesg (SMMU) и версию образа

---

## 6. Что делать

### 6.1 Практическое решение (сейчас)

Форс `VKD3D_FEATURE_LEVEL=12_1` — в Armada готовый механизм:

```json
// system_files/usr/share/armada/game-tweaks.json (дефолт образа)
// или /etc/armada/game-tweaks.json (override на устройстве)
{ "global": { "env": { "VKD3D_FEATURE_LEVEL": "12_1" } } }
```

- `armada-game-launch` экспортирует `env` из game-tweaks в процесс игры как есть
  (`armada-game-launch:169-175`), секция `games.<appid>.env` — per-game
- Оверрайт поднимет уровень и опции ROV/conservative raster независимо от
  драйвера (vkd3d device.c:9936-9938)
- **Риск:** игры, которые реально используют rasterization-order views (редкость),
  не смогут создать пайплайн (в Turnip нет interlock) → ошибка/краш именно в них;
  большинство игр, запрашивающих 1.1/1.2, от ROV не зависят

### 6.2 Правильное решение (упстрим)

Реализовать `VK_EXT_fragment_shader_interlock` в Turnip (upstream Mesa):

1. Экспозиция: features `fragmentShaderPixelInterlock`/`fragmentShaderSampleInterlock`
   + расширение в `tu_device.cc` (features2/список расширений)
2. Пайплайн: поддержка `VK_PIPELINE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS`
3. NIR/ir3: lower интерлок-билтинов (begin/end interlock) и гарантия атомарности
   обращений к attachment'ам между ними относительно других фрагментов
4. Железо: A7xx — TBR; гарантий порядка обработки фрагментов в open-source
   документации нет; проприетарный драйвер Qualcomm поддерживает
   `GL_EXT_fragment_shader_interlock` на ряде чипов (Android), т.е. HW-режим
   вероятно существует, но не описан публично

Это задача уровня месяцев для разработчика драйвера; готовых патчей для порта
нет (проверены: Turnip main, патчи Armada/ROCKNIX/batocera).

Где размещать, если появится: `armada-os/armada-packages/mesa/patches/`
(стандартный workflow Armada: патч поверх Mesa, см. существующие 0001-0003),
либо сначала в upstream Mesa.

### 6.3 Сообщить в проект

Issue в armada-os/armada: «QCS8550/RP6: vkd3d feature level cap» + результаты
команд из раздела 5 — если sparse не даётся, это ядро/рантайм; если даётся,
но 12_0 — запрос interlock в Turnip.

---

## 7. Ссылки на код

- vkd3d: `HansKristian-Work/vkd3d-proton` master, `libs/vkd3d/device.c`
  (9585-9621, 9895-9964, 9211-9212, 4651-4662, 10279-10281)
- Turnip: Mesa main, `src/freedreno/vulkan/tu_device.cc` (336, 468, 486, 1110,
  1345), `tu_knl_drm_msm.cc` (1366-1367, 1401)
- Ядро: `linux-7.2.6`: `include/uapi/drm/msm_drm.h` (95, 119),
  `drivers/gpu/drm/msm/msm_drv.c` (802), `adreno/adreno_gpu.c` (447-449),
  `msm_gpu.h` (295-303), `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c` (393-401),
  `arch/arm64/boot/dts/qcom/{sm8550,qcs8550}.dtsi`
- Armada: `system_files/usr/lib/armada/devices/retroid-pocket-6.conf`,
  `system_files/usr/share/armada/game-tweaks.json`,
  `system_files/usr/libexec/armada/armada-game-launch` (169-175, 249),
  `armada-os/armada-packages/{mesa,kernel}/`
