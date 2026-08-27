#include "dolphin/card.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../internal.hpp"
#include "dolphin/types.h"

#include "../card/CardRawFile.hpp"
#include "../card/DolphinCardPath.hpp"
#include "../logging.hpp"
#include "../card/CardGciFolder.hpp"
#include "../fs_helper.hpp"

/* melee_card_compat.h and melee_pc_harness.h aren't included directly:
 * both live in OpenMelee's own include/ tree, which isn't on this
 * library's include path when aurora is built standalone (e.g. its own
 * CI). Declare just the symbols this file calls, matching melee_main.c's
 * own precedent for a narrow local declaration instead of a cross-tree
 * header. OpenMelee's build still provides the real definitions
 * (src/card_compat.c, src/melee_pc_harness.c); aurora_card only needs to
 * *compile* standalone (e.g. its own CI just builds the library, it
 * doesn't link a final executable), so the missing symbols at link time
 * are harmless outside OpenMelee. */
extern "C" int melee_pc_harness_no_card(void);
extern "C" void melee_card_enqueue_callback(CARDCallback callback, s32 chan, s32 result);

namespace {
aurora::Module Log("aurora::card");
std::array<std::unique_ptr<aurora::card::ICard>, 2> CardChannels = {{}};
std::array<std::filesystem::path, 2> cardPaths;
/* CARDGetXferredBytes is a control-block query on retail. Aurora performs
 * card I/O synchronously, so retain the completed byte count per channel. */
std::array<s32, 2> transferredBytes = {{0, 0}};

constexpr uint16_t CARD_SECTOR_SIZE = 8192;

const char* GetCardRegion() {
  switch (aurora::g_gameName[3]) {
  case 'E':
  default:
    return "USA";
  case 'P':
    return "EUR";
  case 'J':
    return "JAP";
  }
}

#define GET_CARD(slot) CardChannels[slot]
#define CARD_USE_GCI_FOLDER SelectedFileType == CARD_GCIFOLDER
#define CARD_READY(slot) (CARD_USE_GCI_FOLDER ? true : std::filesystem::exists(CardChannels[slot]->cardFilename()))
#define CARD_STUB Log.debug("{} is stubbed.", __FUNCTION__);

bool Initialized = false;
CARDFileType SelectedFileType = CARD_GCIFOLDER;
bool UseFastMode = false;

void CopyKabuFileHandleToDolphin(const s32 chan, const aurora::card::FileHandle& handle, CARDFileInfo* fileInfo) {
  fileInfo->chan = chan;
  fileInfo->fileNo = handle.getFileNo();
  fileInfo->offset = handle.getOffset();
}

aurora::card::FileHandle CreateKabuFileHandleFromDolphin(const CARDFileInfo* fileInfo) {
  return aurora::card::FileHandle{static_cast<u32>(fileInfo->fileNo), fileInfo->offset};
}

std::filesystem::path GetCardFullPath(const std::filesystem::path& path, const aurora::card::ECardSlot slot) {
  if (path.empty())
    return "";

  if (CARD_USE_GCI_FOLDER) {
    return path / GetCardRegion() / (slot == aurora::card::ECardSlot::SlotA ? "Card A" : "Card B");
  } else {
    return path / fmt::format("MemoryCard{}.{}.raw", slot == aurora::card::ECardSlot::SlotA ? "A" : "B", GetCardRegion());
  }
}
} // namespace

extern "C" {

void CopyKabuStatsToDolphin(const aurora::card::CardStat& kabuStats, CARDStat* stats) {
  memcpy(stats->fileName, kabuStats.x0_fileName, std::size(kabuStats.x0_fileName));
  memcpy(stats->gameName, kabuStats.x28_gameName.data(), kabuStats.x28_gameName.size());
  memcpy(stats->company, kabuStats.x2c_company.data(), kabuStats.x2c_company.size());
  memcpy(stats->offsetIcon, kabuStats.x44_offsetIcon.data(), kabuStats.x44_offsetIcon.size());

  stats->length = kabuStats.x20_length;
  stats->time = kabuStats.x24_time;
  stats->bannerFormat = kabuStats.x2e_bannerFormat;
  stats->iconAddr = kabuStats.x30_iconAddr;
  stats->iconFormat = kabuStats.x34_iconFormat;
  stats->iconSpeed = kabuStats.x36_iconSpeed;
  stats->commentAddr = kabuStats.x38_commentAddr;
  stats->offsetBanner = kabuStats.x3c_offsetBanner;
  stats->offsetBannerTlut = kabuStats.x40_offsetBannerTlut;
  stats->offsetIconTlut = kabuStats.x64_offsetIconTlut;
  stats->offsetData = kabuStats.x68_offsetData;
}

void CopyDolphinStatsToKabu(aurora::card::CardStat& kabuStats, const CARDStat* stats) {
  memcpy(kabuStats.x0_fileName, stats->fileName, std::size(kabuStats.x0_fileName));
  memcpy(kabuStats.x28_gameName.data(), stats->gameName, kabuStats.x28_gameName.size());
  memcpy(kabuStats.x2c_company.data(), stats->company, kabuStats.x2c_company.size());
  memcpy(kabuStats.x44_offsetIcon.data(), stats->offsetIcon, kabuStats.x44_offsetIcon.size());

  kabuStats.x20_length = stats->length;
  kabuStats.x24_time = stats->time;
  kabuStats.x2e_bannerFormat = stats->bannerFormat;
  kabuStats.x30_iconAddr = stats->iconAddr;
  kabuStats.x34_iconFormat = stats->iconFormat;
  kabuStats.x36_iconSpeed = stats->iconSpeed;
  kabuStats.x38_commentAddr = stats->commentAddr;
  kabuStats.x3c_offsetBanner = stats->offsetBanner;
  kabuStats.x40_offsetBannerTlut = stats->offsetBannerTlut;
  kabuStats.x64_offsetIconTlut = stats->offsetIconTlut;
  kabuStats.x68_offsetData = stats->offsetData;
}

void CARDDetectDolphin(const s32 chan) {
  if (Initialized) {
    Log.fatal("CARDDetectDolphin() called after CARDInit()!");
  }

  if (chan == 0 || chan == 1) {
    cardPaths[chan] =
        aurora::card::ResolveDolphinCardPath(static_cast<aurora::card::ECardSlot>(chan), GetCardRegion(), CARD_USE_GCI_FOLDER);
    if (cardPaths[chan].empty()) {
      Log.error("Failed to detect Dolphin Card!");
      return;
    }
    Log.info("Detected Dolphin Card at: {}", fs_path_to_string(cardPaths[chan]));
  } else {
    cardPaths[0] = aurora::card::ResolveDolphinCardPath(aurora::card::ECardSlot::SlotA, GetCardRegion(), CARD_USE_GCI_FOLDER);
    cardPaths[1] = aurora::card::ResolveDolphinCardPath(aurora::card::ECardSlot::SlotB, GetCardRegion(), CARD_USE_GCI_FOLDER);

    if (cardPaths[0].empty() && cardPaths[1].empty()) {
      Log.error("Failed to detect Dolphin Card!");
      return;
    }

    Log.info(
      "Detected Dolphin Card at: {} and {}",
      fs_path_to_string(cardPaths[0]),
      fs_path_to_string(cardPaths[1]));
  }
}

void CARDSetBasePath(const char* path, const s32 chan) {
  if (Initialized) {
    Log.fatal("CARDSetBasePath() called after CARDInit()!");
  }

  std::filesystem::path filePath(path);

  if (filePath.has_filename() && !std::filesystem::is_directory(filePath)) {
    filePath = filePath.remove_filename();
    Log.warn("Path supplied a filename, discarding. New Path: {}", fs_path_to_string(filePath));
  }

  if (chan == 0 || chan == 1) {
    cardPaths[chan] = GetCardFullPath(filePath, static_cast<aurora::card::ECardSlot>(chan));
  } else {
    cardPaths[0] = GetCardFullPath(filePath, aurora::card::ECardSlot::SlotA);
    cardPaths[1] = GetCardFullPath(filePath, aurora::card::ECardSlot::SlotB);
  }
}

void CARDSetLoadType(CARDFileType type) {
  SelectedFileType = type;
}

void CARDInit(const char* game, const char* maker) {
  if (Initialized) {
    return;
  }
  Initialized = true;
  Log.info("CARD API Initialized BUILT <{} {}>", __DATE__, __TIME__);

  for (int i = 0; i < CardChannels.size(); ++i) {
    switch (SelectedFileType) {
    case CARD_RAWIMAGE:
      CardChannels[i] = std::make_unique<aurora::card::CardRawFile>();
      break;
    case CARD_GCIFOLDER:
      CardChannels[i] = std::make_unique<aurora::card::CardGciFolder>();
      break;
    }
    CardChannels[i]->InitCard(game, maker);
  }

  std::filesystem::path cardWorkingDir;
  // melee-pc: --nosave gives this process its own private, isolated card
  // directory (under the OS temp dir, keyed by PID) instead of the shared
  // default location. Parallel test instances then each get a real,
  // normally-formatted card of their own rather than racing on the same
  // file -- unlike actually simulating "no card present" (tried first),
  // which triggers a real "no memory card, continue anyway?" prompt that
  // blocks unattended boot, exactly the problem this flag exists to avoid.
  // A launch argument rather than an env var on purpose
  // (melee_pc_harness.h): an env var left set would silently affect every
  // future launch until explicitly unset.
  if (melee_pc_harness_no_card()) {
    cardWorkingDir = std::filesystem::temp_directory_path() /
                      ("melee-pc-card-" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::create_directories(cardWorkingDir, ec);
    // A brand-new, never-used card is a *legitimate* first-boot state on
    // retail too (no save file yet) -- it prompts same as a genuinely
    // missing card, just blocking boot either way. Seed this isolated
    // directory from the real default card (read-only copy) so this
    // process gets its own private copy of an *already-initialized* card
    // instead of a blank one, matching a normal subsequent boot with no
    // prompt at all.
    std::filesystem::path defaultDir = aurora::g_config.userPath != nullptr
        ? std::filesystem::path(reinterpret_cast<const char8_t*>(aurora::g_config.userPath))
        : std::filesystem::current_path();
    std::filesystem::path defaultCard = GetCardFullPath(defaultDir, aurora::card::ECardSlot::SlotA);
    std::filesystem::path isolatedCard = GetCardFullPath(cardWorkingDir, aurora::card::ECardSlot::SlotA);
    if (std::filesystem::exists(defaultCard, ec)) {
      std::filesystem::create_directories(isolatedCard.parent_path(), ec);
      std::filesystem::copy(defaultCard, isolatedCard,
                             std::filesystem::copy_options::recursive |
                                 std::filesystem::copy_options::overwrite_existing,
                             ec);
    }
  } else if (aurora::g_config.userPath != nullptr) {
    cardWorkingDir = reinterpret_cast<const char8_t*>(aurora::g_config.userPath);
  } else {
    cardWorkingDir = std::filesystem::current_path();
  }

  bool loadedCard = false;

  for (int i = 0; i < 2; ++i) {
    // use default working directory if no path was supplied for card
    if (cardPaths[i].empty()) {
      cardPaths[i] = GetCardFullPath(cardWorkingDir, static_cast<aurora::card::ECardSlot>(i));
    }

    const auto& curPath = cardPaths[i];

    std::error_code ec;
    if (std::filesystem::exists(curPath, ec) && CardChannels[i]->open(curPath)) {
      loadedCard = true;
      Log.info("Loaded GC Card Image: {}", fs_path_to_string(curPath));
    } else if (ec) {
      Log.warn("Failed to inspect GC Card path '{}': {}", fs_path_to_string(curPath), ec.message());
    } else if (std::filesystem::exists(curPath, ec)) {
      Log.warn("Failed to load GC Card Image: {}", fs_path_to_string(curPath));
    }
  }

  // create a SlotA card if no cards were loaded
  if (!loadedCard) {
    CardChannels[0]->open(cardPaths[0]);
    CardChannels[0]->format(aurora::card::ECardSlot::SlotA);
    CardChannels[0]->close();
	CardChannels[0]->open(cardPaths[0]);
  }
}

void CARDSetGameAndMaker(const s32 chan, const char* game, const char* maker) {
  if (chan < 0 || chan >= 2) {
    return;
  }

  CardChannels[chan]->setCurrentGame(game);
  CardChannels[chan]->setCurrentMaker(maker);
}

// TODO: Investigate if this is necessary. Do some games throw an error if CARD's fast mode can't be use?
BOOL CARDGetFastMode() { return UseFastMode; }

BOOL CARDSetFastMode(const BOOL enable) {
  const bool oldFast = UseFastMode;
  UseFastMode = enable;
  return oldFast;
}

s32 CARDCheck(const s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);

  return static_cast<s32>(card->getError());
}

s32 CARDCheckAsync(const s32 chan, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  const auto res = static_cast<s32>(card->getError());
  melee_card_enqueue_callback(callback, chan, res);
  return static_cast<s32>(card->getError());
}

s32 CARDCheckEx(const s32 chan, s32* xferBytes [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);

  return static_cast<s32>(card->getError());
}

s32 CARDCheckExAsync(const s32 chan, s32* xferBytes [[maybe_unused]], const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto& card = GET_CARD(chan);
  const auto res = static_cast<s32>(card->getError());
  melee_card_enqueue_callback(callback, chan, res);
  return static_cast<s32>(card->getError());
}

s32 CARDCreate(const s32 chan, const char* fileName, const u32 size, CARDFileInfo* fileInfo) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);

  aurora::card::FileHandle handle;
  auto res = card->createFile(fileName, size, handle);
  if (res == aurora::card::ECardResult::READY) {
    CopyKabuFileHandleToDolphin(chan, handle, fileInfo);
    card->commit();
  } else
    Log.error("Failed to create file: {}", fileName);

  return static_cast<s32>(res);
}

s32 CARDCreateAsync(const s32 chan, const char* fileName, const u32 size, CARDFileInfo* fileInfo,
                    const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDCreate(chan, fileName, size, fileInfo);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDDelete(const s32 chan, const char* fileName) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  const auto res = card->deleteFile(fileName);

  if (res != aurora::card::ECardResult::READY) {
    Log.error("Failed to delete file: {}", fileName);
  } else {
    card->commit();
  }

  return static_cast<s32>(res);
}

s32 CARDDeleteAsync(const s32 chan, const char* fileName, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDDelete(chan, fileName);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDFastDelete(const s32 chan, const s32 fileNo) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  const auto res = card->deleteFile(fileNo);
  if (res != aurora::card::ECardResult::READY) {
    Log.error("Failed to delete file at idx: {}", fileNo);
  } else {
    card->commit();
  }

  return static_cast<s32>(res);
}

s32 CARDFastDeleteAsync(const s32 chan, const s32 fileNo, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDFastDelete(chan, fileNo);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDFastOpen(const s32 chan, const s32 fileNo, CARDFileInfo* fileInfo) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);

  aurora::card::FileHandle handle;
  const auto res = card->openFile(fileNo, handle);
  if (res == aurora::card::ECardResult::READY)
    CopyKabuFileHandleToDolphin(chan, handle, fileInfo);
  else
    Log.error("Failed to open file at idx: {}", fileNo);

  return static_cast<s32>(res);
}

s32 CARDFormat(s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  card->format(static_cast<aurora::card::ECardSlot>(chan));
  card->commit();
  return CARD_RESULT_READY;
}

s32 CARDFormatAsync(const s32 chan, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDFormat(chan);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDFreeBlocks(const s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  card->getFreeBlocks(*byteNotUsed, *filesNotUsed);
  return CARD_RESULT_READY;
}

s32 CARDGetAttributes(const s32 chan, const s32 fileNo [[maybe_unused]], u8* attr [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented: ICard/CardStat here carry no per-file attribute byte
  // to read (see ICard.hpp), so there is nothing real to answer with.
  // Previously returned CARD_RESULT_READY unconditionally, which is a
  // silent lie -- *attr would be left uninitialized garbage that the
  // caller is told to trust. Not currently called anywhere in decomp/src
  // (grep-confirmed), so this can't regress anything live; failing loudly
  // means a future caller finds out immediately instead of trusting
  // garbage. Log.error rather than the quieter CARD_STUB debug line, to
  // stand out if this is ever actually reached.
  Log.error("CARDGetAttributes is not implemented (fileNo={})", fileNo);
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDGetEncoding(const s32 chan, u16* encode) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  card->getEncoding(*encode);
  return CARD_RESULT_READY;
}

s32 CARDGetMemSize(const s32 chan, u16* size) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto& card = GET_CARD(chan);
  if (card == nullptr) {
    return CARD_RESULT_NOCARD;
  }
  // Unlike the attribute/mode stubs, real data for this exists -- the
  // same probeCardFile() CARDProbeEx already uses -- so there is no
  // reason to leave it a lying stub. x4_cardSize is bytes; real hardware
  // returns memory size in Mbits, but nothing in decomp/src reads this
  // (grep-confirmed) so there is no established retail-callsite
  // convention to match here. Bytes is the least surprising choice if
  // that ever changes.
  const aurora::card::ProbeResults probeData = card->probeCardFile(cardPaths[chan]);
  if (probeData.x0_error != aurora::card::ECardResult::READY) {
    return static_cast<s32>(probeData.x0_error);
  }
  *size = static_cast<u16>(probeData.x4_cardSize);
  return CARD_RESULT_READY;
}

s32 CARDGetResultCode(const s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  return static_cast<s32>(card->getError());
}

s32 CARDGetSectorSize(const s32 chan, u32* size) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  *size = CARD_SECTOR_SIZE;
  return CARD_RESULT_READY;
}

s32 CARDGetSerialNo(const s32 chan, u64* serialNo) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;

  const auto& card = GET_CARD(chan);
  card->getSerial(*serialNo);
  return CARD_RESULT_READY;
}

s32 __CARDGetStatusEx(const s32 chan, const s32 fileNo [[maybe_unused]], CARDDir* dirent [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented: no raw CARDDir-shaped directory-entry accessor
  // exists on ICard (see ICard.hpp; CardStat is the closest thing, and
  // this asks for the raw on-disk entry, not the translated stat). Not
  // currently called anywhere in decomp/src (grep-confirmed). Same
  // reasoning as CARDGetAttributes just above: fail loudly instead of
  // handing back an unfilled `*dirent` dressed up as CARD_RESULT_READY.
  Log.error("__CARDGetStatusEx is not implemented (fileNo={})", fileNo);
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDGetStatus(const s32 chan, s32 fileNo, CARDStat* stat) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);

  aurora::card::CardStat kabuStat;
  const auto res = card->getStatus(fileNo, kabuStat);
  if (res == aurora::card::ECardResult::READY) {
    CopyKabuStatsToDolphin(kabuStat, stat);
  } else if (res == aurora::card::ECardResult::NOFILE) {
    // Callers (e.g. melee's save-file directory scan in lbcardnew.c) probe every
    // slot on the card looking for files; an empty slot returning NOFILE is the
    // expected, common case, not a failure worth an error log.
    Log.debug("No file at idx: {}", fileNo);
  } else {
    Log.error("Failed to get status of file at idx: {}", fileNo);
  }

  return static_cast<s32>(res);
}

s32 CARDGetXferredBytes(const s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  return transferredBytes[chan];
}
// these two funcs are out of scope for aurora::card. stubbed for now
s32 CARDMount(const s32 chan, void* workArea [[maybe_unused]], CARDCallback detachCallback [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  
  return CARD_RESULT_READY;
}
s32 CARDMountAsync(const s32 chan, void* workArea [[maybe_unused]], const CARDCallback detachCallback [[maybe_unused]],
                   const CARDCallback attachCallback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Real hardware's attachCallback fires once mounting completes; decomp
  // callers (e.g. melee/lb/lbcardnew.c's lb_8001A184) submit-then-mark-busy
  // around this call and rely on the callback firing later to clear that
  // busy state (see src/card_compat.c). This stub previously never called
  // it at all, so that busy flag was set and never cleared -- the caller's
  // poll loop (`do { } while (lb_8001B6F8() == 0xB)`) spun forever.
  if (attachCallback != nullptr) {
    melee_card_enqueue_callback(attachCallback, chan, CARD_RESULT_READY);
  }
  return CARD_RESULT_READY;
}

s32 CARDOpen(const s32 chan, const char* fileName, CARDFileInfo* fileInfo) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);

  aurora::card::FileHandle handle;
  const auto res = card->openFile(fileName, handle);
  if (res == aurora::card::ECardResult::READY)
    CopyKabuFileHandleToDolphin(chan, handle, fileInfo);
  else
    Log.error("Failed to open file: {}", fileName);

  return static_cast<s32>(res);
}

BOOL CARDProbe(const s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto& card = GET_CARD(chan);
  if (card == nullptr) {
    return FALSE;
  }

  aurora::card::ProbeResults probeData = card->probeCardFile(cardPaths[chan]);
  return probeData.x0_error != aurora::card::ECardResult::NOCARD ? TRUE : FALSE;
}

s32 CARDProbeEx(const s32 chan, s32* memSize, s32* sectorSize) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto& card = GET_CARD(chan);
  if (card == nullptr) {
    if (memSize != nullptr) *memSize = 0;
    if (sectorSize != nullptr) *sectorSize = 0;
    return CARD_RESULT_NOCARD;
  }

  // ReSharper disable once CppUseStructuredBinding
  const aurora::card::ProbeResults probeData = card->probeCardFile(cardPaths[chan]);
  *memSize = probeData.x4_cardSize;
  *sectorSize = probeData.x8_sectorSize;

  return static_cast<s32>(probeData.x0_error);
}

s32 CARDRename(const s32 chan, const char* oldName, const char* newName) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);

  return static_cast<s32>(card->renameFile(oldName, newName));
}

s32 CARDRenameAsync(const s32 chan, const char* oldName, const char* newName, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDRename(chan, oldName, newName);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDSetAttributesAsync(const s32 chan, s32 fileNo [[maybe_unused]], u8 attr [[maybe_unused]],
                           CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented, same reasoning as CARDSetAttributes below -- but
  // unlike that one, this MUST still fire the callback even on failure:
  // decomp callers (see CARDMountAsync above / src/card_compat.c) submit-
  // then-mark-busy and rely on the callback to clear that busy state, so
  // a stub that never calls it hangs them forever regardless of what
  // result it reports. Reporting the honest failure here (rather than
  // CARD_RESULT_READY) at least lets a future caller's error handling
  // run instead of believing an attribute write happened.
  Log.error("CARDSetAttributesAsync is not implemented (fileNo={})", fileNo);
  if (callback != nullptr) {
    melee_card_enqueue_callback(callback, chan, CARD_RESULT_FATAL_ERROR);
  }
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDSetAttributes(const s32 chan, s32 fileNo [[maybe_unused]], u8 attr [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented: same gap as CARDGetAttributes above -- no attribute
  // byte exists anywhere in ICard/CardStat to write. Not currently called
  // anywhere in decomp/src (grep-confirmed). Previously returned
  // CARD_RESULT_READY, silently discarding the write instead of failing;
  // fail loudly instead.
  Log.error("CARDSetAttributes is not implemented (fileNo={})", fileNo);
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDSetStatus(const s32 chan, s32 fileNo, const CARDStat* stat) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);

  aurora::card::CardStat kabuStat;
  CopyDolphinStatsToKabu(kabuStat, stat);
  auto res = card->setStatus(fileNo, kabuStat);
  if (res != aurora::card::ECardResult::READY) {
    Log.error("Failed to set status of file at idx: {}", fileNo);
  } else {
    card->commit();
  }

  return static_cast<s32>(res);
}

s32 __CARDSetStatusEx(const s32 chan, s32 fileNo [[maybe_unused]], CARDDir* dirent [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented, same gap as __CARDGetStatusEx above: no raw
  // CARDDir-shaped write path on ICard. Not currently called anywhere in
  // decomp/src (grep-confirmed). Fail loudly instead of pretending a
  // directory-entry write happened.
  Log.error("__CARDSetStatusEx is not implemented (fileNo={})", fileNo);
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDSetStatusAsync(const s32 chan, const s32 fileNo, const CARDStat* stat, const CARDCallback callback) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  const auto res = CARDSetStatus(chan, fileNo, stat);
  melee_card_enqueue_callback(callback, chan, res);
  return res;
}

s32 CARDUnmount(const s32 chan) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Mounting here (CARDMountAsync above) never actually attaches/detaches
  // anything -- CardChannels[chan] is constructed once in CARDInit and
  // stays valid for the process lifetime, there is no real "unmounted"
  // state to enter. Returning CARD_RESULT_NOCARD claimed the opposite of
  // that: lbcardnew.c's lb_80019CB0 (the only caller, after every full
  // mount/check/open/... task chain) discards this return value today, so
  // it's currently inert, but the value itself was a lie -- the channel
  // is still perfectly ready to use. CARD_READY(chan) is unaffected by
  // mount state either way (see its definition above), so this is a
  // truthful no-op rather than a functional change.
  if (!CARD_READY(chan))
    return CARD_RESULT_NOCARD;
  return CARD_RESULT_READY;
}

s32 CARDGetCurrentMode(const s32 chan, u32* mode [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  // Not implemented: no compatibility-mode concept tracked anywhere in
  // ICard. Not currently called anywhere in decomp/src (grep-confirmed).
  // Previously returned CARD_RESULT_NOCARD unconditionally, which reads
  // as "no memory card present" -- actively misleading for a card that
  // is otherwise mounted and working, and indistinguishable from a real
  // no-card condition to any caller checking this return value. Using
  // FATAL_ERROR instead, consistent with the other not-implemented stubs
  // in this file, so "unimplemented" and "no card" aren't confusable.
  Log.error("CARDGetCurrentMode is not implemented");
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDCancel(CARDFileInfo* fileInfo [[maybe_unused]]) {
  // Not implemented: no async-operation-in-flight concept exists to
  // cancel (every CARD*Async call here already completes synchronously
  // before returning, see CARDReadAsync/CARDWriteAsync/etc. above). Same
  // CARD_RESULT_NOCARD-is-misleading issue as CARDGetCurrentMode above.
  Log.error("CARDCancel is not implemented");
  return CARD_RESULT_FATAL_ERROR;
}

s32 CARDClose(CARDFileInfo* fileInfo) {
  if (fileInfo->chan < 0 || fileInfo->chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(fileInfo->chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(fileInfo->chan);

  auto handle = CreateKabuFileHandleFromDolphin(fileInfo);
  const auto res = card->closeFile(handle);

  if (res != aurora::card::ECardResult::READY)
    Log.error("Failed to close file at idx: {}", fileInfo->fileNo);

  return static_cast<s32>(res);
}

s32 CARDRead(const CARDFileInfo* fileInfo, void* addr, s32 length, const s32 offset) {
  if (fileInfo->chan < 0 || fileInfo->chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(fileInfo->chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(fileInfo->chan);

  aurora::card::FileHandle handle = CreateKabuFileHandleFromDolphin(fileInfo);

  card->seek(handle, offset, aurora::card::SeekOrigin::Begin);
  auto res = card->fileRead(handle, addr, length);
  transferredBytes[fileInfo->chan] =
      res == aurora::card::ECardResult::READY ? length : 0;

  if (res != aurora::card::ECardResult::READY)
    Log.error("Failed to read {} bytes from card", length);

  return static_cast<s32>(res);
}

s32 CARDReadAsync(const CARDFileInfo* fileInfo, void* addr, const s32 length, const s32 offset,
                  const CARDCallback callback) {
  const auto res = CARDRead(fileInfo, addr, length, offset);
  if (callback != nullptr) {
    melee_card_enqueue_callback(callback, fileInfo->chan, res);
  }
  return res;
}

s32 CARDWrite(const CARDFileInfo* fileInfo, const void* addr, const s32 length, const s32 offset) {
  if (fileInfo->chan < 0 || fileInfo->chan >= 2) {
    return CARD_RESULT_FATAL_ERROR;
  }
  if (!CARD_READY(fileInfo->chan))
    return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(fileInfo->chan);

  aurora::card::FileHandle handle = CreateKabuFileHandleFromDolphin(fileInfo);

  card->seek(handle, offset, aurora::card::SeekOrigin::Begin);
  auto res = card->fileWrite(handle, addr, length);
  transferredBytes[fileInfo->chan] =
      res == aurora::card::ECardResult::READY ? length : 0;

  if (res != aurora::card::ECardResult::READY) {
    Log.error("Failed to write {} bytes to card", length);
  } else {
    card->commit();
  }

  return static_cast<s32>(res);
}

s32 CARDWriteAsync(const CARDFileInfo* fileInfo, const void* addr, const s32 length, const s32 offset,
                   const CARDCallback callback) {
  const auto res = CARDWrite(fileInfo, addr, length, offset);
  if (callback != nullptr) {
    melee_card_enqueue_callback(callback, fileInfo->chan, res);
  }
  return res;
}
}
