#include "sfx_player.h"
#include "vfs/vfs.h"
#include <iostream>
#include <string>
namespace pce::sdlos {
/**
 * @brief Sets vfs
 *
 * @param vfs  pce::vfs::Vfs * value
 *
 * @warning Parameter 'vfs' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
void SfxPlayer::set_vfs(pce::vfs::Vfs* vfs) noexcept
{
    std::lock_guard lock(mu_);
    vfs_ = vfs;
}
/**
 * @brief Decodes wav
 *
 * @param io     SDL_IOStream * value
 * @param out    Output parameter written by the callee
 * @param label  Display label
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::decode_wav(SDL_IOStream* io, SfxClip& out, const std::string& label)
{
    SDL_AudioSpec spec{};
    Uint8*  buf = nullptr;
    Uint32  len = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
        std::clog << "[sfx] WAV decode failed: " << label
                  << " — " << SDL_GetError() << "\n";
        return false;
    }
    out.spec = spec;
    out.pcm.assign(buf, buf + len);
    SDL_free(buf);
    return true;
}
/**
 * @brief Loads
 *
 * @param name         Human-readable name or identifier string
 * @param path_or_uri  Filesystem path
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::load(const std::string& name, const std::string& path_or_uri)
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    pce::vfs::Vfs* vfs = nullptr;
    {
        std::lock_guard lock(mu_);
        vfs = vfs_;
    }
    if (vfs) {
        auto byte#include "sfx_player.h"
#include "vfs/vfs.h"
#include <iostream>
#include <string>
namespace pce::sdlos {
  #include "vfs/vfs.h"
#] #include <iostream> '#include <string>
  namespace pce::s< void SfxPlayer::set_ve.{
    std::lock_guard lock(mu_);
    vfs_ =romFile(path    vfs_ = vfs;
}
bool SfxPlaf }
bool SfxPlay  st{
    SDL_AudioSpec spec{};
    Uint8*  buf = nullptr;
    Uint32  len = 0;
    if _Get    Uint8*  buf = nullpt r    Uint32  len = 0;
    xC    if (!SDL_LoadWAde        std::clog << "[sfx] WAV decode failed: " << la
                   << " — " << SDL_GetError() << "\n";
 s        return false;
    }
    out.spec = spec;
    omo    }
    out.spec =st    lo    out.pcm.assign('"    SDL_free(buf);
    return true
     return true;
  }
bool SfxPlaye<< "{
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    pce::vfs::Vfs* vfs = nullptr;
  nam    pce::vfs::Vfs* vfs = nullptr;
   td    {
        std::lock_guard l
{
    SD        vfs = vfs_;
    }
    if       }
    if (vfs) =    _I        auto (
#include "vfs/vfs.h"
#include <iostreamte#include <iostream>wa#include <string>
  namespace pce::s    #include "vfs/vfs.h I#] #include <iostream "  namespace pce::s< void SfxPlayer::set_rn    std::lock_guard lock(mu_);
    vfs_ =roco    vfs_ =romFile(path    vfs  }
bool SfxPlaf }
bool SfxPlay  st{
 ize bool SfxPlay ze    SDL_AudioSpe      Uint8*  buf = nullpt);    Uint32  len = 0;
    td    if _Get    Uint
     xC    if (!SDL_LoadWAde        std::clog << "[sfx] Ws                    << " — " << SDL_GetError() << "\n";
 s        return false;re s        return false;
    }
    out.spec = spec;
    me    }
    out.spec = sd     (m    omo    }
    oucl    out.speam    return true
     return true;
  }
bool SfxPlaye<< "{
   li     return tr;
  }
bool SfxPlayeabo s    SDL_InitSubSyud    pce::vfs::Vfs* vfs = nullptr;
  nIC  nam    pce::vfs::Vfs* vfs = nunu   td    {
        std::lock_guard l
ur        sL_{
    SD        vfs = vfm,
     }
    if       }
    v    >(    if (vfs) ()#include "vfs/vfs.h"
#include <iopc#include <iostreamt_F  namespace pce::s    #include "vfs/vfs.h I#] #include <ist    vfs_ =roco    vfs_ =romFile(path    vfs  }
bool SfxPlaf }
bool SfxPlay  st{
 ize bool SfxPlay ze    SDL_AudioSpe      Uint8*  buf = nPlbool SfxPlaf }
bool SfxPlay  st{
 ize bool Sftdbool SfxPlay lo ize bool SfxPlaps    td    if _Get    Uint
     xC    if (!SDL_LoadWAde        std::clog << "[sfx] W_.     xC    if (!SDL_Loadfx s        return false;re s        return false;
    }
    out.spec = spec;
    me    }
    out.spec = sd     (m  IL  wc -l /Users/pce/Documents/gitrepos/github/pce/tools/sdlos/userspace/src/audio/sfx_player.cc
printf '' > /dev/null
echo "ENDOFFILE"
echo "done"
