/*----------------------------------------------------------------------------/
  Lovyan GFX library - LCD graphics library .
  
  support platform:
    ESP32 (SPI/I2S) with Arduino/ESP-IDF
    ATSAMD51 (SPI) with Arduino
  
Original Source:  
 https://github.com/lovyan03/LovyanGFX/  

Licence:  
 [BSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)  

Author:  
 [lovyan03](https://twitter.com/lovyan03)  

Contributors:  
 [ciniml](https://github.com/ciniml)  
 [mongonta0716](https://github.com/mongonta0716)  
 [tobozo](https://github.com/tobozo)  
/----------------------------------------------------------------------------*/
#ifndef LGFX_SPI_ESP32_HPP_
#define LGFX_SPI_ESP32_HPP_

#include <cstring>
#include <type_traits>

#include <driver/periph_ctrl.h>
#include <driver/rtc_io.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <soc/rtc.h>
#include <soc/spi_reg.h>
#include <soc/spi_struct.h>

#if defined (ARDUINO) // Arduino ESP32
 #include <SPI.h>
 #include <driver/periph_ctrl.h>
 #include <soc/periph_defs.h>
 #include <esp32-hal-cpu.h>
#else
 #include <esp_log.h>
 #include <driver/spi_master.h>
 #if ESP_IDF_VERSION_MAJOR > 3
  #include <driver/spi_common_internal.h>
 #endif

#endif

#include "esp32_common.hpp"
#include "../LGFX_Device.hpp"

namespace lgfx
{
  inline static void spi_dma_transfer_active(int dmachan)
  {
    spicommon_dmaworkaround_transfer_active(dmachan);
  }

  #define MEMBER_DETECTOR(member, classname, classname_impl, valuetype) struct classname_impl { \
  template<class T, valuetype V> static constexpr std::integral_constant<valuetype, T::member> check(decltype(T::member)*); \
  template<class T, valuetype V> static constexpr std::integral_constant<valuetype, V> check(...); \
  };template<class T, valuetype V> class classname : public decltype(classname_impl::check<T, V>(nullptr)) {};
  MEMBER_DETECTOR(spi_host   , get_spi_host   , get_spi_host_impl   , spi_host_device_t)
  MEMBER_DETECTOR(spi_mosi   , get_spi_mosi   , get_spi_mosi_impl   , int)
  MEMBER_DETECTOR(spi_miso   , get_spi_miso   , get_spi_miso_impl   , int)
  MEMBER_DETECTOR(spi_sclk   , get_spi_sclk   , get_spi_sclk_impl   , int)
  MEMBER_DETECTOR(spi_dlen   , get_spi_dlen   , get_spi_dlen_impl   , int)
  MEMBER_DETECTOR(dma_channel, get_dma_channel, get_dma_channel_impl, int)
  #undef MEMBER_DETECTOR

  template <class CFG>
  class LGFX_SPI : public LGFX_Device
  {
  public:

    virtual ~LGFX_SPI() {
      if ((0 != _dma_channel) && _dmadesc) {
        heap_free(_dmadesc);
        _dmadesc = nullptr;
        _dmadesc_len = 0;
      }
      delete_dmabuffer();
    }

    LGFX_SPI() : LGFX_Device()
    {
    }

    void init(int sclk, int miso, int mosi, spi_host_device_t host = VSPI_HOST)
    {
      _spi_sclk = sclk;
      _spi_miso = miso;
      _spi_mosi = mosi;
      _spi_host = host;

      init_impl();
    }

    __attribute__ ((always_inline)) inline void begin(int sclk, int miso, int mosi, spi_host_device_t host = VSPI_HOST) { init(sclk, miso, mosi, host); }

    __attribute__ ((always_inline)) inline void begin(void) { init_impl(); }

    __attribute__ ((always_inline)) inline void init(void) { init_impl(); }

    void writeCommand(std::uint_fast8_t cmd) override { write_cmd(cmd); }

    void writeData(std::uint_fast8_t data) override { if (_spi_dlen == 16) { write_data(data << 8, _spi_dlen); } else { write_data(data, _spi_dlen); } }

    std::uint32_t readCommand(std::uint_fast8_t commandByte, std::uint_fast8_t index=0, std::uint_fast8_t len=4) override { startWrite(); auto res = read_command(commandByte, index << 3, len << 3); endWrite(); return res; }

    void initBus(void) override
    {
      preInit();
      spi::init(_spi_host, _spi_sclk, _spi_miso, _spi_mosi, _dma_channel);
    }

    void releaseBus(void)
    {
      lgfxPinMode(_spi_mosi, pin_mode_t::output);
      lgfxPinMode(_spi_miso, pin_mode_t::output);
      lgfxPinMode(_spi_sclk, pin_mode_t::output);
      spi::release(_spi_host);
    }

//----------------------------------------------------------------------------
  protected:

    void preInit(void) override
    {
      _spi_host = get_spi_host<CFG, VSPI_HOST>::value;
      _spi_port = (_spi_host == HSPI_HOST) ? 2 : 3;  // FSPI=1  HSPI=2  VSPI=3;
      _spi_w0_reg = reg(SPI_W0_REG(_spi_port));
      _spi_cmd_reg = reg(SPI_CMD_REG(_spi_port));
      _spi_user_reg = reg(SPI_USER_REG(_spi_port));
      _spi_mosi_dlen_reg = reg(SPI_MOSI_DLEN_REG(_spi_port));
      _spi_dma_out_link_reg = reg(SPI_DMA_OUT_LINK_REG(_spi_port));
    }

    void preCommandList(void) override
    {
      wait_spi();
      if (!_fill_mode) return;
      _fill_mode = false;
      set_clock_write();
    }

    void postCommandList(void) override
    {
      wait_spi();
    }

    void postSetPanel(void) override
    {
      _last_apb_freq = -1;
      _cmd_ramwr      = _panel->getCmdRamwr();
      _len_setwindow  = _panel->len_setwindow;

      if (_panel->spi_dlen >> 3) _spi_dlen = _panel->spi_dlen & ~7;
      //fpGetWindowAddr = _len_setwindow == 32 ? PanelCommon::getWindowAddr32 : PanelCommon::getWindowAddr16;

      std::int32_t spi_dc = _panel->spi_dc;
      _mask_reg_dc = (spi_dc < 0) ? 0 : (1 << (spi_dc & 31));

      _gpio_reg_dc_l = get_gpio_lo_reg(spi_dc);
      _gpio_reg_dc_h = get_gpio_hi_reg(spi_dc);
      *_gpio_reg_dc_h = _mask_reg_dc;
      lgfxPinMode(spi_dc, pin_mode_t::output);

      cs_h();
      lgfxPinMode(_panel->spi_cs, pin_mode_t::output);

      postSetRotation();
      postSetColorDepth();
    }

    void postSetRotation(void) override
    {
      bool fullscroll = (_sx == 0 && _sy == 0 && _sw == _width && _sh == _height);
      /*
      _cmd_caset = _panel->getCmdCaset();
      _cmd_raset = _panel->getCmdRaset();
      _colstart  = _panel->getColStart();
      _rowstart  = _panel->getRowStart();
      //*/
      _width     = _panel->getWidth();
      _height    = _panel->getHeight();
      _clip_r = _width - 1;
      _clip_b = _height - 1;

      if (fullscroll) {
        _sw = _width;
        _sh = _height;
      }
      _clip_l = _clip_t = 0;
    }

    void beginTransaction_impl(void) override {
      if (_in_transaction) return;
      _in_transaction = true;
      begin_transaction();
    }

    void begin_transaction(void) {
      _fill_mode = false;
      std::uint32_t apb_freq = getApbFrequency();
      if (_last_apb_freq != apb_freq) {
        _last_apb_freq = apb_freq;
        _clkdiv_read  = FreqToClockDiv(apb_freq, _panel->freq_read);
        _clkdiv_fill  = FreqToClockDiv(apb_freq, _panel->freq_fill);
        _clkdiv_write = FreqToClockDiv(apb_freq, _panel->freq_write);
      }

      auto spi_mode = _panel->spi_mode;
      _user = (spi_mode == 1 || spi_mode == 2) ? SPI_CK_OUT_EDGE | SPI_USR_MOSI : SPI_USR_MOSI;
      std::uint32_t pin = (spi_mode & 2) ? SPI_CK_IDLE_EDGE : 0;

      spi::beginTransaction(_spi_host);

      if (_dma_channel) {
        _next_dma_reset = true;
      }

      *_spi_user_reg = _user;
      *reg(SPI_PIN_REG(_spi_port))  = pin;
      set_clock_write();

      cs_l();
      if (nullptr != _panel->fp_begin) { _panel->fp_begin(_panel, this); }
    }

    void endTransaction_impl(void) override {
      if (!_in_transaction) return;
      _in_transaction = false;
      end_transaction();
    }

    void end_transaction(void) {
      if (nullptr != _panel->fp_end) { _panel->fp_end(_panel, this); }
      if (_spi_dlen == 16 && (_align_data)) write_data(0, 8);
      if (_panel->spi_cs < 0) {
        write_cmd(0); // NOP command
      }
      dc_h();
      spi::endTransaction(_spi_host, _panel->spi_cs);
#if defined (ARDUINO) // Arduino ESP32
      *_spi_user_reg = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN; // for other SPI device (SD)
#endif
    }

    void initDMA_impl(void) override
    {
      if (_dma_channel) {
        spi_dma_reset();
      }
    }

    void waitDMA_impl(void) override
    {
      wait_spi();
    }

    bool dmaBusy_impl(void) override
    {
      return *_spi_cmd_reg & SPI_USR;
    }

    void writePixelsDMA_impl(const void* data, std::int32_t length) override
    {
      write_bytes((const std::uint8_t*)data, length * _write_conv.bytes, true);
    }

    void writeBytes_impl(const std::uint8_t* data, std::int32_t length, bool use_dma) override
    {
      write_bytes((const std::uint8_t*)data, length, use_dma);
    }

    void setWindow_impl(std::int32_t xs, std::int32_t ys, std::int32_t xe, std::int32_t ye) override
    {
      if (_fill_mode) {
        _fill_mode = false;
        wait_spi();
        set_clock_write();
      }
      set_window(xs, ys, xe, ye);
      write_cmd(_cmd_ramwr);
    }

    void drawPixel_impl(std::int32_t x, std::int32_t y) override
    {
      if (!_panel->fp_fillRect) {
        if (_in_transaction) {
          if (_fill_mode) {
            _fill_mode = false;
            wait_spi();
            set_clock_write();
          }
          set_window(x, y, x, y);
          write_cmd(_cmd_ramwr);
          write_data(_color.raw, _write_conv.bits);
          return;
        }

        begin_transaction();
        set_window(x, y, x, y);
        write_cmd(_cmd_ramwr);
        write_data(_color.raw, _write_conv.bits);
        end_transaction();
      }
      else
      {
        if (_in_transaction) _panel->fp_fillRect(_panel, this, x, y, 1, 1, _color.raw);
        else
        {
          begin_transaction();
          _panel->fp_fillRect(_panel, this, x, y, 1, 1, _color.raw);
          end_transaction();
        }
      }
    }

    void writeFillRect_impl(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
      if (!_panel->fp_fillRect) {
        if (_fill_mode) {
          _fill_mode = false;
          wait_spi();
          set_clock_write();
        }
        set_window(x, y, x+w-1, y+h-1);
        write_cmd(_cmd_ramwr);
        push_block(w*h, _clkdiv_write != _clkdiv_fill);
      }
      else
      {
        _panel->fp_fillRect(_panel, this, x, y, w, h, _color.raw);
      }
    }

    void pushBlock_impl(std::int32_t length) override
    {
      if (_panel->fp_pushBlock)
      {
        _panel->fp_pushBlock(_panel, this, length, _color.raw);
      }
      else
      {
        push_block(length);
      }
    }

    void push_block(std::int32_t length, bool fillclock = false)
    {
      auto bits = _write_conv.bits;
      std::uint32_t regbuf0 = _color.raw;
      if (length == 1) { write_data(regbuf0, bits); return; }

      length *= bits;          // convert to bitlength.
      std::uint32_t len = std::min(96, length); // 1st send length = max 12Byte (96bit). 
      bool bits16 = (bits == 16);

      std::uint32_t regbuf1;
      std::uint32_t regbuf2;
      // make 12Bytes data.
      if (bits16) {
        regbuf0 = regbuf0 | regbuf0 << 16;
        regbuf1 = regbuf0;
        regbuf2 = regbuf0;
      } else {
        if (_spi_dlen == 16 && length & 8) _align_data = !_align_data;
        regbuf0 = regbuf0      | regbuf0 << 24;
        regbuf1 = regbuf0 >> 8 | regbuf0 << 16;
        regbuf2 = regbuf0 >>16 | regbuf0 <<  8;
      }

      auto spi_w0_reg = _spi_w0_reg;
      dc_h();

      // copy to SPI buffer register
      spi_w0_reg[0] = regbuf0;
      spi_w0_reg[1] = regbuf1;
      spi_w0_reg[2] = regbuf2;

      set_write_len(len);

      if (fillclock) {
        set_clock_fill();  // fillmode clockup
        _fill_mode = true;
      }

      exec_spi();   // 1st send.
      if (0 == (length -= len)) return;

      std::uint32_t regbuf[7];
      regbuf[0] = regbuf0;
      regbuf[1] = regbuf1;
      regbuf[2] = regbuf2;
      regbuf[3] = regbuf0;
      regbuf[4] = regbuf1;
      regbuf[5] = regbuf2;
      regbuf[6] = regbuf0;

      // copy to SPI buffer register
      memcpy((void*)&spi_w0_reg[3], regbuf, 24);
      memcpy((void*)&spi_w0_reg[9], regbuf, 28);

      // limit = 64Byte / depth_bytes;
      // When 24bit color, 504 bits out of 512bit buffer are used.
      // When 16bit color, it uses exactly 512 bytes. but, it behaves like a ring buffer, can specify a larger size.
      std::uint32_t limit;
      if (bits16) {
        limit = (1 << 11);
        len = length & (limit - 1);
      } else {
        limit = 504;
        len = length % limit;
      }
      if (len) {
        wait_spi();
        set_write_len(len);
        exec_spi();                // 2nd send.
        if (0 == (length -= len)) return;
      }
      wait_spi();
      set_write_len(limit);
      exec_spi();
      while (length -= limit) {
        taskYIELD();
        wait_spi();
        exec_spi();
      }
    }

    void write_cmd(std::uint_fast8_t cmd)
    {
      if (_spi_dlen == 16) {
        if (_align_data) write_data(0, 8);
        cmd <<= 8;
      }
      std::uint32_t len = _spi_dlen - 1;
      auto spi_mosi_dlen_reg = _spi_mosi_dlen_reg;
      auto spi_w0_reg        = _spi_w0_reg;
      dc_l();
      *spi_mosi_dlen_reg = len;
      *spi_w0_reg = cmd;
      exec_spi();
    }

    void write_data(std::uint32_t data, std::uint32_t bit_length)
    {
      auto spi_w0_reg        = _spi_w0_reg;
      auto spi_mosi_dlen_reg = _spi_mosi_dlen_reg;
      dc_h();
      *spi_mosi_dlen_reg = bit_length - 1;
      *spi_w0_reg = data;
      exec_spi();
      if (_spi_dlen == 16 && (bit_length & 8)) _align_data = !_align_data;
    }

    __attribute__ ((always_inline)) inline 
    void write_cmd_data(const std::uint8_t* addr)
    {
      auto spi_mosi_dlen_reg = _spi_mosi_dlen_reg;
      auto spi_w0_reg        = _spi_w0_reg;
      if (_spi_dlen == 16 && _align_data)
      {
        _align_data = false;
        dc_h();
        *spi_mosi_dlen_reg = 8 - 1;
        *spi_w0_reg = 0;
        exec_spi();
      }

      do {
        std::uint32_t data = *addr++;
        if (_spi_dlen == 16) {
          data <<= 8;
        }
        std::uint32_t len = _spi_dlen - 1;
        dc_l();
        *spi_mosi_dlen_reg = len;
        *spi_w0_reg = data;
        exec_spi();
//        write_cmd(*addr++);
        std::uint_fast8_t numArgs = *addr++;
        if (numArgs)
        {
          data = *reinterpret_cast<const std::uint32_t*>(addr);
          if (_spi_dlen == 16)
          {
            if (numArgs > 2)
            {
              std::uint_fast8_t len = ((numArgs + 1) >> 1) + 1;
              std::uint_fast8_t i = 1;
              do
              {
                _spi_w0_reg[i] = addr[i * 2] << 8 | addr[i * 2 + 1] << 24;
              } while (++i != len);
            }
            data = (data & 0xFF) << 8 | (data >> 8) << 24;
            //write_data(addr[0] << 8 | addr[1] << 24, _spi_dlen * numArgs);
          }
          else
          {
            if (numArgs > 4)
            {
              memcpy((void*)&_spi_w0_reg[1], addr + 4, numArgs - 4);
            }
            //write_data(*reinterpret_cast<const std::uint32_t*>(addr), _spi_dlen * numArgs);
          }
          addr += numArgs;
          len = _spi_dlen * numArgs - 1;
          dc_h();
          *spi_mosi_dlen_reg = len;
          *spi_w0_reg = data;
          exec_spi();
        }
      } while (reinterpret_cast<const std::uint16_t*>(addr)[0] != 0xFFFF);
    }

    void set_window(std::uint_fast16_t xs, std::uint_fast16_t ys, std::uint_fast16_t xe, std::uint_fast16_t ye)
    {
//*
      std::uint8_t buf[16];
      if (auto b = _panel->getWindowCommands1(buf, xs, ys, xe, ye)) { write_cmd_data(b); }
      if (auto b = _panel->getWindowCommands2(buf, xs, ys, xe, ye)) { write_cmd_data(b); }
      return;
/*/
      std::uint32_t len;
      if (_spi_dlen == 8) {
        len = _len_setwindow - 1;
      } else {
        len = (_len_setwindow << 1) - 1;
        if (_align_data) write_data(0, 8);
      }
      auto spi_mosi_dlen_reg = _spi_mosi_dlen_reg;
      auto fp = fpGetWindowAddr;

      if (_xs != xs || _xe != xe) {
        auto cmd = _cmd_caset;
        if (_spi_dlen == 16) cmd <<= 8;
        std::uint32_t l  = _spi_dlen - 1;
        auto spi_w0_reg  = _spi_w0_reg;
        auto spi_cmd_reg = _spi_cmd_reg;
        dc_l();
        *spi_mosi_dlen_reg = l;
        *spi_w0_reg = cmd;
        *spi_cmd_reg = SPI_USR;

        std::uint32_t tmp = _colstart;

        tmp = fp(xs + tmp, xe + tmp);
        if (_spi_dlen == 16) {
          auto t = tmp >> 16;
          spi_w0_reg[1] = (t & 0xFF) << 8 | (t >> 8) << 24;
          tmp = (tmp & 0xFF) << 8 | (tmp >> 8) << 24;
        }
        dc_h();
        *spi_w0_reg = tmp;
        *spi_mosi_dlen_reg = len;
        *spi_cmd_reg = SPI_USR;
        _xs = xs;
        _xe = xe;
      }
      if (_ys != ys || _ye != ye) {
        auto cmd = _cmd_raset;
        if (_spi_dlen == 16) cmd <<= 8;
        std::uint32_t l  = _spi_dlen - 1;
        auto spi_w0_reg  = _spi_w0_reg;
        auto spi_cmd_reg = _spi_cmd_reg;
        dc_l();
        *spi_mosi_dlen_reg = l;
        *spi_w0_reg = cmd;
        *spi_cmd_reg = SPI_USR;

        std::uint32_t tmp = _rowstart;

        tmp = fp(ys + tmp, ye + tmp);
        if (_spi_dlen == 16) {
          auto t = tmp >> 16;
          spi_w0_reg[1] = (t & 0xFF) << 8 | (t >> 8) << 24;
          tmp = (tmp & 0xFF) << 8 | (tmp >> 8) << 24;
        }
        dc_h();
        *spi_w0_reg = tmp;
        *spi_mosi_dlen_reg = len;
        *spi_cmd_reg = SPI_USR;
        _ys = ys;
        _ye = ye;
      }
//*/
    }

    void start_read(void) {
      if (_spi_dlen == 16 && (_align_data)) write_data(0, 8);

      _fill_mode = false;
      std::uint32_t user = ((_panel->spi_mode_read == 1 || _panel->spi_mode_read == 2) ? SPI_CK_OUT_EDGE | SPI_USR_MISO : SPI_USR_MISO)
                    | (_panel->spi_3wire ? SPI_SIO : 0);
      std::uint32_t pin = (_panel->spi_mode_read & 2) ? SPI_CK_IDLE_EDGE : 0;
      dc_h();
      *_spi_user_reg = user;
      *reg(SPI_PIN_REG(_spi_port)) = pin;
      set_clock_read();
    }

    void end_read(void)
    {
      std::uint32_t pin = (_panel->spi_mode & 2) ? SPI_CK_IDLE_EDGE : 0;
      wait_spi();
      cs_h();
      *_spi_user_reg = _user;
      *reg(SPI_PIN_REG(_spi_port)) = pin;
      if (_panel->spi_cs < 0) {
        write_cmd(0); // NOP command
      }
      set_clock_write();
      _fill_mode = false;

      cs_l();
    }

    std::uint32_t read_data(std::uint32_t length)
    {
      set_read_len(length);
      exec_spi();
      wait_spi();
      return *_spi_w0_reg;

    }

    std::uint32_t read_command(std::uint_fast8_t command, std::uint32_t bitindex = 0, std::uint32_t bitlen = 8)
    {
      bitindex += _panel->len_dummy_read_rddid;
      startWrite();
      write_cmd(command);
      start_read();
      if (bitindex) read_data(bitindex);
      std::uint32_t res = read_data(bitlen);
      end_read();
      endWrite();
      return res;
    }

    void pushImage_impl(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, pixelcopy_t* param, bool use_dma) override
    {
      if (_panel->fp_pushImage != nullptr)
      {
        _panel->fp_pushImage(_panel, this, x, y, w, h, param);
      }
      else
      {
        push_image(x, y, w, h, param, use_dma);
      }
    }

    void push_image(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, pixelcopy_t* param, bool use_dma)
    {
      auto bits = _write_conv.bits;
      auto src_x = param->src_x;
      auto fp_copy = param->fp_copy;

      std::int32_t xr = (x + w) - 1;
      std::int32_t whb = w * h * bits >> 3;
      if (param->transp == ~0) {
        if (param->no_convert) {
          setWindow_impl(x, y, xr, y + h - 1);
          std::uint32_t i = (src_x + param->src_y * param->src_bitwidth) * bits >> 3;
          auto src = &((const std::uint8_t*)param->src_data)[i];
          if (_dma_channel && use_dma) {
            if (param->src_bitwidth == w) {
              _setup_dma_desc_links(src, w * h * bits >> 3);
            } else {
              _setup_dma_desc_links(src, w * bits >> 3, h, param->src_bitwidth * bits >> 3);
            }
            dc_h();
            set_write_len(whb << 3);
            *_spi_dma_out_link_reg = SPI_OUTLINK_START | ((int)(&_dmadesc[0]) & 0xFFFFF);
            spi_dma_transfer_active(_dma_channel);
            exec_spi();
            return;
          }
          if (param->src_bitwidth == w || h == 1) {
            if (_dma_channel && !use_dma && (64 < whb) && (whb <= 1024)) {
              auto buf = get_dmabuffer(whb);
              memcpy(buf, src, whb);
              write_bytes(buf, whb, true);
            } else {
              write_bytes(src, whb, use_dma);
            }
          } else {
            auto add = param->src_bitwidth * bits >> 3;
            do {
              write_bytes(src, w * bits >> 3, use_dma);
              src += add;
            } while (--h);
          }
        } else
        if (_dma_channel && (64 < whb)) {
          if (param->src_bitwidth == w && (whb <= 1024)) {
            auto buf = get_dmabuffer(whb);
            fp_copy(buf, 0, w * h, param);
            setWindow_impl(x, y, xr, y + h - 1);
            write_bytes(buf, whb, true);
          } else {
            std::int32_t wb = w * bits >> 3;
            auto buf = get_dmabuffer(wb);
            fp_copy(buf, 0, w, param);
            setWindow_impl(x, y, xr, y + h - 1);
            write_bytes(buf, wb, true);
            while (--h) {
              param->src_x = src_x;
              param->src_y++;
              buf = get_dmabuffer(wb);
              fp_copy(buf, 0, w, param);
              write_bytes(buf, wb, true);
            }
          }
        } else {
          setWindow_impl(x, y, xr, y + h - 1);
          do {
            write_pixels(w, param);
            param->src_x = src_x;
            param->src_y++;
          } while (--h);
        }
      } else {
        auto fp_skip = param->fp_skip;
        h += y;
        do {
          std::int32_t i = 0;
          while (w != (i = fp_skip(i, w, param))) {
            auto buf = get_dmabuffer(w * bits >> 3);
            std::int32_t len = fp_copy(buf, 0, w - i, param);
            setWindow_impl(x + i, y, x + i + len - 1, y);
            write_bytes(buf, len * bits >> 3, true);
            if (w == (i += len)) break;
          }
          param->src_x = src_x;
          param->src_y++;
        } while (++y != h);
      }
    }

    void writePixels_impl(std::int32_t length, pixelcopy_t* param) override
    {
      if (!_panel->fp_writePixels)
      {
        if (_dma_channel)
        {
          const std::uint8_t dst_bytes = _write_conv.bytes;
          std::uint32_t limit = (dst_bytes == 2) ? 16 : 12;
          std::uint32_t len;
          do {
            len = ((length - 1) % limit) + 1;
            //if (limit <= 256) limit <<= 2;
            if (limit <= 512) limit <<= 1;
            auto dmabuf = get_dmabuffer(len * dst_bytes);
            param->fp_copy(dmabuf, 0, len, param);
            write_bytes(dmabuf, len * dst_bytes, true);
          } while (length -= len);
        }
        else
        {
          write_pixels(length, param);
        }
      }
      else
      {
        _panel->fp_writePixels(_panel, this, length, param);
      }
    }

    void write_pixels(std::int32_t length, pixelcopy_t* param)
    {
      const std::uint8_t bytes = _write_conv.bytes;
      const std::uint32_t limit = (bytes == 2) ? 16 : 10; //  limit = 32/bytes (bytes==2 is 16   bytes==3 is 10)
      std::uint32_t len = (length - 1) / limit;
      std::uint32_t highpart = (len & 1) << 3;
      len = length - (len * limit);
      std::uint32_t regbuf[8];
      param->fp_copy(regbuf, 0, len, param);

      auto spi_w0_reg = _spi_w0_reg;

      std::uint32_t user_reg = *_spi_user_reg;

      dc_h();
      set_write_len(len * bytes << 3);

      memcpy((void*)&spi_w0_reg[highpart], regbuf, (len * bytes + 3) & (~3));
      if (highpart) *_spi_user_reg = user_reg | SPI_USR_MOSI_HIGHPART;
      exec_spi();
      if (0 == (length -= len)) return;

      for (; length; length -= limit) {
        param->fp_copy(regbuf, 0, limit, param);
        memcpy((void*)&spi_w0_reg[highpart ^= 0x08], regbuf, limit * bytes);
        std::uint32_t user = user_reg;
        if (highpart) user |= SPI_USR_MOSI_HIGHPART;
        if (len != limit) {
          len = limit;
          wait_spi();
          set_write_len(limit * bytes << 3);
          *_spi_user_reg = user;
          exec_spi();
        } else {
          wait_spi();
          *_spi_user_reg = user;
          exec_spi();
        }
      }
    }

    void write_bytes(const std::uint8_t* data, std::int32_t length, bool use_dma = false)
    {
      if (length <= 64) {
        auto spi_w0_reg = _spi_w0_reg;
        dc_h();
        set_write_len(length << 3);
        memcpy((void*)spi_w0_reg, data, (length + 3) & (~3));
        exec_spi();
        return;
      } else if (_dma_channel && use_dma) {
        dc_h();
        set_write_len(length << 3);
        _setup_dma_desc_links(data, length);
        *_spi_dma_out_link_reg = SPI_OUTLINK_START | ((int)(&_dmadesc[0]) & 0xFFFFF);
        spi_dma_transfer_active(_dma_channel);
        exec_spi();
        return;
      }
      constexpr std::uint32_t limit = 32;
      std::uint32_t len = ((length - 1) & 0x1F) + 1;
      std::uint32_t highpart = ((length - 1) & limit) >> 2; // 8 or 0

      auto spi_w0_reg = _spi_w0_reg;

      std::uint32_t user_reg = *_spi_user_reg;
      dc_h();
      set_write_len(len << 3);

      memcpy((void*)&spi_w0_reg[highpart], data, (len + 3) & (~3));
      if (highpart) *_spi_user_reg = user_reg | SPI_USR_MOSI_HIGHPART;
      exec_spi();
      if (0 == (length -= len)) return;

      for (; length; length -= limit) {
        data += len;
        memcpy((void*)&spi_w0_reg[highpart ^= 0x08], data, limit);
        std::uint32_t user = user_reg;
        if (highpart) user |= SPI_USR_MOSI_HIGHPART;
        if (len != limit) {
          len = limit;
          wait_spi();
          set_write_len(limit << 3);
          *_spi_user_reg = user;
          exec_spi();
        } else {
          wait_spi();
          *_spi_user_reg = user;
          exec_spi();
        }
      }
    }

    void readRect_impl(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, void* dst, pixelcopy_t* param) override
    {
      if (!_panel->fp_readRect)
      {
        startWrite();
        set_window(x, y, x + w - 1, y + h - 1);
        auto len = w * h;
        if (!_panel->spi_read)
        {
          memset(dst, 0, len * param->dst_bits >> 3);
        }
        else
        {
          write_cmd(_panel->getCmdRamrd());
          std::uint32_t len_dummy_read_pixel = _panel->len_dummy_read_pixel;
          start_read();
          if (len_dummy_read_pixel) {;
            set_read_len(len_dummy_read_pixel);
            exec_spi();
          }

          if (param->no_convert) {
            read_bytes((std::uint8_t*)dst, len * _read_conv.bytes);
          } else {
            read_pixels(dst, len, param);
          }
          end_read();
        }
        endWrite();
      }
      else
      {
        _panel->fp_readRect(_panel, this, x, y, w, h, dst, param);
      }
    }

    void read_pixels(void* dst, std::int32_t length, pixelcopy_t* param)
    {
      std::int32_t len1 = std::min(length, 10); // 10 pixel read
      std::int32_t len2 = len1;
      auto len_read_pixel  = _read_conv.bits;
      std::uint32_t regbuf[8];
      wait_spi();
      set_read_len(len_read_pixel * len1);
      exec_spi();
      param->src_data = regbuf;
      std::int32_t dstindex = 0;
      std::uint32_t highpart = 8;
      std::uint32_t userreg = *_spi_user_reg;
      auto spi_w0_reg = _spi_w0_reg;
      do {
        if (0 == (length -= len1)) {
          len2 = len1;
          wait_spi();
          *_spi_user_reg = userreg;
        } else {
          std::uint32_t user = userreg;
          if (highpart) user = userreg | SPI_USR_MISO_HIGHPART;
          if (length < len1) {
            len1 = length;
            wait_spi();
            set_read_len(len_read_pixel * len1);
          } else {
            wait_spi();
          }
          *_spi_user_reg = user;
          exec_spi();
        }
        memcpy(regbuf, (void*)&spi_w0_reg[highpart ^= 8], len2 * len_read_pixel >> 3);
        param->src_x = 0;
        dstindex = param->fp_copy(dst, dstindex, dstindex + len2, param);
      } while (length);
    }

    void read_bytes(std::uint8_t* dst, std::int32_t length, bool use_dma = false)
    {
      if (_dma_channel && use_dma) {
        wait_spi();
        set_read_len(length << 3);
        _setup_dma_desc_links(dst, length);
        *reg(SPI_DMA_IN_LINK_REG(_spi_port)) = SPI_INLINK_START | ((int)(&_dmadesc[0]) & 0xFFFFF);
        spi_dma_transfer_active(_dma_channel);
        exec_spi();
      } else {
        std::int32_t len1 = std::min(length, 32);  // 32 Byte read.
        std::int32_t len2 = len1;
        wait_spi();
        set_read_len(len1 << 3);
        exec_spi();
        std::uint32_t highpart = 8;
        std::uint32_t userreg = *_spi_user_reg;
        auto spi_w0_reg = _spi_w0_reg;
        do {
          if (0 == (length -= len1)) {
            len2 = len1;
            wait_spi();
            *_spi_user_reg = userreg;
          } else {
            std::uint32_t user = userreg;
            if (highpart) user = userreg | SPI_USR_MISO_HIGHPART;
            if (length < len1) {
              len1 = length;
              wait_spi();
              set_read_len(len1 << 3);
            } else {
              wait_spi();
            }
            *_spi_user_reg = user;
            exec_spi();
          }
          memcpy(dst, (void*)&spi_w0_reg[highpart ^= 8], len2);
          dst += len2;
        } while (length);
      }
//*/
    }

    static void _alloc_dmadesc(size_t len)
    {
      if (_dmadesc) heap_caps_free(_dmadesc);
      _dmadesc_len = len;
      _dmadesc = (lldesc_t*)heap_caps_malloc(sizeof(lldesc_t) * len, MALLOC_CAP_DMA);
    }

    static void spi_dma_reset(void)
    {
      periph_module_reset( PERIPH_SPI_DMA_MODULE );
      _next_dma_reset = false;
    }

    static void _setup_dma_desc_links(const std::uint8_t *data, std::int32_t len)
    {          //spicommon_setup_dma_desc_links
      if (!_dma_channel) return;

      if (_next_dma_reset) {
        spi_dma_reset();
      }
      if (_dmadesc_len * SPI_MAX_DMA_LEN < len) {
        _alloc_dmadesc(len / SPI_MAX_DMA_LEN + 1);
      }
      lldesc_t *dmadesc = _dmadesc;

      while (len > SPI_MAX_DMA_LEN) {
        len -= SPI_MAX_DMA_LEN;
        dmadesc->buf = (std::uint8_t *)data;
        data += SPI_MAX_DMA_LEN;
        *(std::uint32_t*)dmadesc = SPI_MAX_DMA_LEN | SPI_MAX_DMA_LEN<<12 | 0x80000000;
        dmadesc->qe.stqe_next = dmadesc + 1;
        dmadesc++;
      }
      *(std::uint32_t*)dmadesc = ((len + 3) & ( ~3 )) | len << 12 | 0xC0000000;
      dmadesc->buf = (std::uint8_t *)data;
      dmadesc->qe.stqe_next = nullptr;
    }

    static void _setup_dma_desc_links(const std::uint8_t *data, std::int32_t w, std::int32_t h, std::int32_t width)
    {          //spicommon_setup_dma_desc_links
      if (!_dma_channel) return;

      if (_next_dma_reset) {
        spi_dma_reset();
      }
      if (_dmadesc_len < h) {
        _alloc_dmadesc(h);
      }
      lldesc_t *dmadesc = _dmadesc;
      std::int32_t idx = 0;
      do {
        dmadesc[idx].buf = (std::uint8_t *)data;
        data += width;
        *(std::uint32_t*)(&dmadesc[idx]) = ((w + 3) & (~3)) | w<<12 | 0x80000000;
        dmadesc[idx].qe.stqe_next = &dmadesc[idx + 1];
      } while (++idx < h);
      --idx;
      dmadesc[idx].eof = 1;
//    *(std::uint32_t*)(&dmadesc[idx]) |= 0xC0000000;
      dmadesc[idx].qe.stqe_next = 0;
    }

    static void _setup_dma_desc_links(std::uint8_t** data, std::int32_t w, std::int32_t h, bool endless)
    {          //spicommon_setup_dma_desc_links
      if (!_dma_channel) return;

      if (_next_dma_reset) {
        spi_dma_reset();
      }

      if (_dmadesc_len < h) {
        _alloc_dmadesc(h);
      }

      lldesc_t *dmadesc = _dmadesc;
      std::int32_t idx = 0;
      do {
        dmadesc[idx].buf = (std::uint8_t *)data[idx];
        *(std::uint32_t*)(&dmadesc[idx]) = w | w<<12 | 0x80000000;
        dmadesc[idx].qe.stqe_next = &dmadesc[idx + 1];
      } while (++idx < h);
      --idx;
      if (endless) {
        dmadesc[idx].qe.stqe_next = &dmadesc[0];
      } else {
        dmadesc[idx].eof = 1;
//        *(std::uint32_t*)(&dmadesc[idx]) |= 0xC0000000;
        dmadesc[idx].qe.stqe_next = 0;
      }
    }

    __attribute__ ((always_inline)) inline volatile std::uint32_t* reg(std::uint32_t addr) { return (volatile std::uint32_t *)ETS_UNCACHED_ADDR(addr); }
    __attribute__ ((always_inline)) inline void set_clock_write(void) { *reg(SPI_CLOCK_REG(_spi_port)) = _clkdiv_write; }
    __attribute__ ((always_inline)) inline void set_clock_read(void)  { *reg(SPI_CLOCK_REG(_spi_port)) = _clkdiv_read;  }
    __attribute__ ((always_inline)) inline void set_clock_fill(void)  { *reg(SPI_CLOCK_REG(_spi_port)) = _clkdiv_fill;  }
    __attribute__ ((always_inline)) inline void exec_spi(void) {        *_spi_cmd_reg = SPI_USR; }
    __attribute__ ((always_inline)) inline void wait_spi(void) { while (*_spi_cmd_reg & SPI_USR); }
    __attribute__ ((always_inline)) inline void set_write_len(std::uint32_t bitlen) { *_spi_mosi_dlen_reg = bitlen - 1; }
    __attribute__ ((always_inline)) inline void set_read_len( std::uint32_t bitlen) { *reg(SPI_MISO_DLEN_REG(_spi_port)) = bitlen - 1; }

    __attribute__ ((always_inline)) inline void dc_h(void) {
      auto mask_reg_dc = _mask_reg_dc;
      auto gpio_reg_dc_h = _gpio_reg_dc_h;
      wait_spi();
      *gpio_reg_dc_h = mask_reg_dc;
    }
    __attribute__ ((always_inline)) inline void dc_l(void) {
      auto mask_reg_dc = _mask_reg_dc;
      auto gpio_reg_dc_l = _gpio_reg_dc_l;
      wait_spi();
      *gpio_reg_dc_l = mask_reg_dc;
    }

/*
    __attribute__ ((always_inline)) inline void cs_h(void) { *_gpio_reg_cs_h = _mask_reg_cs; }
    __attribute__ ((always_inline)) inline void cs_l(void) { *_gpio_reg_cs_l = _mask_reg_cs; }
//
    __attribute__ ((always_inline)) inline void cs_h(void) { if (_mask_reg_cs) *_gpio_reg_cs_h = _mask_reg_cs; else cs_h_impl(); }
    __attribute__ ((always_inline)) inline void cs_l(void) { if (_mask_reg_cs) *_gpio_reg_cs_l = _mask_reg_cs; else cs_l_impl(); }
    virtual void cs_h_impl(void) {}
    virtual void cs_l_impl(void) {}
//*/

    int _spi_mosi = get_spi_mosi<CFG, -1>::value;
    int _spi_miso = get_spi_miso<CFG, -1>::value;
    int _spi_sclk = get_spi_sclk<CFG, -1>::value;
    int _spi_dlen = get_spi_dlen<CFG,  8>::value;
    spi_host_device_t _spi_host;
    static constexpr int _dma_channel= get_dma_channel<CFG,  0>::value;
    /*
    std::uint32_t(*fpGetWindowAddr)(std::uint_fast16_t, std::uint_fast16_t);
    std::uint_fast16_t _colstart;
    std::uint_fast16_t _rowstart;
    std::uint_fast16_t _xs;
    std::uint_fast16_t _xe;
    std::uint_fast16_t _ys;
    std::uint_fast16_t _ye;
    std::uint32_t _cmd_caset;
    std::uint32_t _cmd_raset;
    //*/
    std::uint32_t _cmd_ramwr;

  private:
    std::uint32_t _last_apb_freq;
    std::uint32_t _clkdiv_write;
    std::uint32_t _clkdiv_read;
    std::uint32_t _clkdiv_fill;
    std::uint32_t _len_setwindow;
    bool _fill_mode;
    bool _align_data = false;
    std::uint32_t _mask_reg_dc;
    volatile std::uint32_t* _gpio_reg_dc_h;
    volatile std::uint32_t* _gpio_reg_dc_l;
    volatile std::uint32_t* _spi_w0_reg;
    volatile std::uint32_t* _spi_cmd_reg;
    volatile std::uint32_t* _spi_user_reg;
    volatile std::uint32_t* _spi_mosi_dlen_reg;
    volatile std::uint32_t* _spi_dma_out_link_reg;
    std::uint32_t _user;
    std::uint8_t _spi_port;
    static lldesc_t* _dmadesc;
    static std::uint32_t _dmadesc_len;
    static bool _next_dma_reset;

  };
  template <class T> lldesc_t* LGFX_SPI<T>::_dmadesc = nullptr;
  template <class T> std::uint32_t LGFX_SPI<T>::_dmadesc_len = 0;
  template <class T> bool LGFX_SPI<T>::_next_dma_reset;

//----------------------------------------------------------------------------

}

using lgfx::LGFX_SPI;

#endif
