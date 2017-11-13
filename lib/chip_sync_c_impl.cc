/* -*- c++ -*- */
/* 
 * Copyright 2017 <+YOU OR YOUR COMPANY+>.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "chip_sync_c_impl.h"
#include <volk/volk.h>
#include <gnuradio/math.h>

namespace gr {
  namespace wifi_dsss {
    #define d_debug 0
    #define dout d_debug && std::cout
    #define TWO_PI M_PI*2.0f
    static const float d_barker[11]={1,-1,1,1,-1,1,1,1,-1,-1,-1};
    static const uint8_t d_dqpsk_2m_map[4] = {0x00,0x02,0x03,0x01};
    static const uint8_t d_cck_dqpsk_map[2][4] = {
                                                  {0x00,0x02,0x03,0x01},
                                                  {0x03,0x01,0x00,0x02}
                                                };
    chip_sync_c::sptr
    chip_sync_c::make(bool longPre,float threshold)
    {
      return gnuradio::get_initial_sptr
        (new chip_sync_c_impl(longPre, threshold));
    }

    /*
     * The private constructor
     */
    chip_sync_c_impl::chip_sync_c_impl(bool longPre,float threshold)
      : gr::block("chip_sync_c",
              gr::io_signature::make(1, 1, sizeof(gr_complex)),
              gr::io_signature::make(0, 0, 0)),
              d_preType(longPre),
              d_psdu_out(pmt::mp("psdu_out"))
    {
      d_sync_word = (longPre)? 0x05CF : 0xF3A0;
      message_port_register_out(d_psdu_out);
      enter_search();
      d_chip_buf = (gr_complex*) volk_malloc(sizeof(gr_complex)*64,volk_get_alignment());
      set_history(11);
      if(threshold<0){
        throw std::invalid_argument("Threshold should be positive number");
      }else if(threshold>11){
        d_threshold = 11.0;
      }else{
        d_threshold = threshold;
      }
    }

    /*
     * Our virtual destructor.
     */
    chip_sync_c_impl::~chip_sync_c_impl()
    {
      volk_free(d_chip_buf);
    }

    void
    chip_sync_c_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = noutput_items + history();
    }
    void
    chip_sync_c_impl::enter_search()
    {
      d_chip_sync = false;
      d_rx_state = SEARCH;
      d_prev_sym = gr_complex(1.0,0);
    }
    void
    chip_sync_c_impl::enter_sync()
    {
      d_hdr_buf = 0;
      d_hdr_reg = 0x00000000;
      d_hdr_crc = 0x0000;
      d_bit_cnt = 0;
      d_rx_state = SYNC;
    }
    void
    chip_sync_c_impl::enter_psdu()
    {
      d_bit_cnt =0;
      d_rx_state = PSDU;
      d_psdu_sym_cnt =0;
      d_psdu_bit_cnt =0;
    }
    uint8_t
    chip_sync_c_impl::descrambler(const uint8_t& rawbit)
    {
      uint8_t x1=(d_des_state>>3) & 0x01;
      uint8_t x2=(d_des_state>>6) & 0x01;
      d_des_state = (d_des_state<<1) | rawbit;
      return x1^ x2 ^ rawbit;
    }
    bool
    chip_sync_c_impl::check_hdr()
    {
      //dout<<std::hex<<",header:"<<(int)d_hdr_reg<<" ,crc:"<<(int)d_hdr_crc<<std::dec<<std::endl;
      // crc check
      uint16_t crc_init = 0xffff;
      for(int i=0;i<32;++i){
        uint16_t newBit = (d_hdr_reg>>i) & 0x0001;
        uint16_t newLsb = (crc_init>>15) ^ newBit;
        uint16_t tmpXor = (newLsb)? 0x0810 : 0x0000;
        crc_init ^= tmpXor;
        crc_init = (crc_init<<1) | newLsb;
        //dout<<std::hex<<"nb="<<(int)newBit<<",reg="<<(int)crc_init<<std::dec<<std::endl;
      }
      //dout<<std::hex<<"crc_init="<<(int)crc_init<<" hdr="<<(int)d_hdr_crc<<" xor="<<(int)(crc_init^d_hdr_crc)<<std::dec<<std::endl;
      if( (crc_init ^d_hdr_crc) == 0xffff){
        // crc passed
        //dout<<"crc check passed"<<std::endl;
      }else{
        return false;
      }
      // crc passed
      // reading header information
      d_sig_dec = d_hdr_reg & 0xff;
      d_service_dec = (d_hdr_reg >> 8) & 0xff;
      d_length_dec = (d_hdr_reg>>16) & 0xffff;
      //dout<<"Read HDR...sig,service,length="<<std::hex<<(int)d_sig_dec<<","<<(int)d_service_dec<<","<<(int)d_length_dec<<std::dec<<std::endl;
      // find signal field
      if(d_sig_dec == 0x0A){
        if(!d_preType){
          return false;
        }else{
          // 1m
          d_rate_val = 1.0;
          d_psdu_bytes_len = (int) floor(d_length_dec/8.0);
          d_psdu_chip_size = 11;
          d_psdu_type = LONG1M;
        }
      }else if(d_sig_dec == 0x14){
        // 2m
        d_rate_val = 2.0;
        d_psdu_bytes_len = (int) floor(d_length_dec * d_rate_val/8.0);
        d_psdu_chip_size = 11;
        d_psdu_type = DSSS2M;
      }else if(d_sig_dec == 0x37){
        // 5.5m
        d_rate_val = 5.5;
        d_psdu_bytes_len =(int) floor(d_length_dec * d_rate_val/8.0);
        d_psdu_chip_size = 8;
        d_psdu_type = CCK5_5M;
      }else if(d_sig_dec == 0x6e){
        // 11m
        d_rate_val = 11;
        // first check service length field
        d_psdu_bytes_len = (int) floor(d_length_dec * 11/8.0);
        // check here
        d_psdu_bytes_len = (d_service_dec>>6 & 0x01)? d_psdu_bytes_len-1 : d_psdu_bytes_len;
        d_psdu_chip_size = 8;
        d_psdu_type = CCK11M;
      }else{
        return false;
      }
      return true;
    }
    uint8_t
    chip_sync_c_impl::ppdu_get_symbol(const gr_complex* in)
    {
      gr_complex autoVal,diff;
      float phase_diff;
      volk_32fc_32f_dot_prod_32fc(&autoVal,in,d_barker,11);
      if(abs(autoVal)>=d_threshold){
        diff = autoVal * conj(d_prev_sym);
        d_prev_sym = autoVal;
        phase_diff = atan2(diff.imag(),diff.real())/(0.5*M_PI);
        return (abs(phase_diff)>1.0)? 0x01 : 0x00;
      }
      return 0xff;
    }
    uint16_t
    chip_sync_c_impl::psdu_get_symbol(const gr_complex* in,bool isEven)
    { 
      float max_corr = 0;
      uint8_t max_idx =0;
      gr_complex tmpVal,holdVal,diff;
      float phase_diff;
      switch(d_psdu_type){
        case LONG1M:
          volk_32fc_32f_dot_prod_32fc(&tmpVal, in,d_barker,11);
          if(abs(tmpVal)>=d_threshold){
            diff = tmpVal * conj(d_prev_sym);
            d_prev_sym = tmpVal;
            phase_diff = atan2(diff.imag(),diff.real())/(0.5*M_PI);
            return (abs(phase_diff)>1.0)? 0x0001 : 0x0000;
          }
          return 0xffff;
        break;
        case DSSS2M:
          volk_32fc_32f_dot_prod_32fc(&tmpVal, in,d_barker,11);
          if(abs(tmpVal)>=d_threshold){
            diff = tmpVal * conj(d_prev_sym);
            d_prev_sym = tmpVal;
            phase_diff = atan2(diff.imag(),diff.real());
            phase_diff = (phase_diff<0)? phase_diff + TWO_PI : phase_diff;
            max_idx = (uint8_t) round(phase_diff/(0.5*M_PI))%4;
            return (uint16_t)d_dqpsk_2m_map[max_idx];
          }
          return 0xffff;
        break;
        case CCK5_5M:
          for(int i=0;i<4;++i){
            volk_32fc_x2_conjugate_dot_prod_32fc(&holdVal, in, d_cck5_5_chips[i],8);
            if(abs(holdVal)>max_corr){
              max_corr = abs(holdVal);
              max_idx = (uint8_t) i;
            }
          }
          tmpVal = holdVal;
          if(abs(tmpVal)>= d_threshold*(8.0/11.0)){
            diff = tmpVal * conj(d_prev_sym);
            d_prev_sym = tmpVal;
            phase_diff = atan2(diff.imag(),diff.real());
            phase_diff = (phase_diff<0)? phase_diff + TWO_PI : phase_diff;
            uint8_t cckd01 = (uint8_t) round(phase_diff/(0.5*M_PI))%4;
            cckd01 = (isEven)? d_cck_dqpsk_map[0][cckd01] : d_cck_dqpsk_map[1][cckd01];
            return (uint16_t) cckd01 | (max_idx<<2);
          }
          return 0xffff;
        break;
        case CCK11M:
          for(int i=0;i<64;++i){
            volk_32fc_x2_conjugate_dot_prod_32fc(&holdVal,in,d_cck11_chips[i],8);
            if(abs(holdVal)>max_corr){
              max_corr = abs(holdVal);
              max_idx = (uint8_t) i;
            }
          }
          tmpVal = holdVal;
          if(abs(tmpVal)>= d_threshold*(8.0/11.0)){
            diff = tmpVal * conj(d_prev_sym);
            d_prev_sym = tmpVal;
            phase_diff = atan2(diff.imag(),diff.real());
            phase_diff = (phase_diff<0)? phase_diff + TWO_PI : phase_diff;
            uint8_t cckd01 = (uint8_t) round(phase_diff/(0.5*M_PI))%4;
            cckd01 = (isEven)? d_cck_dqpsk_map[0][cckd01] : d_cck_dqpsk_map[1][cckd01];
            return (uint16_t) cckd01 | (max_idx<<2);
          }
          return 0xffff;
        break;
        default:
          return 0xffff;
        break;
      }
    }
    void
    chip_sync_c_impl::psdu_write_bits(const uint16_t& outByte)
    {
      uint8_t boffset = d_psdu_bit_cnt %8;
      uint8_t bpos = d_psdu_bit_cnt/8;
      uint8_t curByte = outByte & 0x00ff;
      uint8_t debit;
      d_buf[bpos] = (boffset==0)? 0x00:d_buf[bpos];
      switch(d_psdu_type){
        case LONG1M:
          debit = descrambler(curByte);
          d_buf[bpos] |= (debit<<boffset);
          d_psdu_bit_cnt++;
        break;
        case DSSS2M:
          for(int i=0;i<2;++i){
            debit = descrambler( (curByte>>i) & 0x01 );
            d_buf[bpos] |= (debit<<boffset++);
          }
          d_psdu_bit_cnt+=2;
        break;
        case CCK5_5M:
          for(int i=0;i<4;++i){
            debit = descrambler( (curByte>>i) & 0x01 );
            d_buf[bpos] |= (debit<<boffset++);
          }
          d_psdu_bit_cnt+=4;
        break;
        case CCK11M:
          for(int i=0;i<8;++i){
            debit = descrambler( (curByte>>i) & 0x01 );
            d_buf[bpos] |= (debit<<boffset++);
          }
          d_psdu_bit_cnt+=8;
        break;
        default:
          throw std::runtime_error("undefined state");
        break;
      }
    }
    int
    chip_sync_c_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      const gr_complex *in = (const gr_complex *) input_items[0];
      int nin = ninput_items[0]-history();
      int ncon = 0;
      gr_complex autoVal,diff;
      float phase_diff;
      uint8_t tmpbit;
      while(ncon<nin){
        switch(d_rx_state){
          case SEARCH:
            while(ncon<nin){
              if(!d_chip_sync){
                volk_32fc_32f_dot_prod_32fc(&autoVal,&in[ncon++],d_barker,11);
                if(abs(autoVal)>=d_threshold){
                  //dout<<"at search state, sync a barker sequence. val="<<abs(autoVal)<<", thres="<<d_threshold<<std::endl;
                  d_chip_sync = true;
                  d_chip_cnt = 0;
                  d_prev_sym = autoVal;
                }
              }else{
                d_chip_cnt++;
                if(d_chip_cnt==11){
                  d_chip_cnt =0;
                  tmpbit = ppdu_get_symbol(&in[ncon++]);
                  if(tmpbit==0xff){
                    //dout<<"Search failed set chip sync to false"<<std::endl;
                    d_chip_sync= false;
                    d_prev_sym = gr_complex(1.0,0);
                  }else{
                    uint8_t deBit = descrambler(tmpbit);
                    d_sync_reg = (d_sync_reg<<1) | deBit;
                    //dout<<std::hex<<(int)d_sync_reg<<std::dec<<std::endl;
                    if(d_sync_reg == d_sync_word){
                      //dout<<"At SEARCH, found sync word, change state"<<std::endl;
                      enter_sync();
                      break;
                    }
                  }
                }else{
                  ncon++;
                }
              }
            }
          break;
          case SYNC:
            while(ncon<nin){
              d_chip_cnt++;
              if(d_chip_cnt==11){
                d_chip_cnt =0;
                tmpbit = ppdu_get_symbol(&in[ncon++]);
                if(tmpbit == 0xff){
                  enter_search();
                  break;
                }else{
                  uint8_t deBit = descrambler(tmpbit);
                  //dout<<"debits="<<std::hex<<(int)deBit<<std::dec<<std::endl;
                  if(d_bit_cnt<32){
                    d_hdr_reg |= (deBit<<d_bit_cnt);
                  }else{
                    d_hdr_crc |= (deBit<<(d_bit_cnt-32));
                  }
                  //d_hdr_buf = (d_hdr_buf<<1)|deBit;
                  d_bit_cnt++;
                  if(d_bit_cnt == 48){
                    d_bit_cnt =0;
                    // including crc
                    if(check_hdr()){
                      //dout<<"At state sync, header check passed"<<std::endl;
                      enter_psdu();
                      break;
                    }else{
                      //dout<<"At state sync, header check failed"<<std::endl;
                      enter_search();
                      break;
                    }
                  }
                }
              }else{
                ncon++;
              }
            }
          break;
          case PSDU:
            while(ncon<nin){
              d_chip_cnt++;
              if(d_chip_cnt == d_psdu_chip_size){
                d_chip_cnt =0;
                uint16_t outByte= psdu_get_symbol(&in[ncon++],( (d_psdu_sym_cnt++)%2 == 0));
                if(outByte==0xffff){
                  enter_search();
                  break;
                }else{
                  // insert bits into buffer 
                  psdu_write_bits(outByte);
                  if(d_psdu_bit_cnt==d_psdu_bytes_len*8){
                    // complete reception
                    pmt::pmt_t psdu_msg = pmt::make_blob(d_buf,d_psdu_bytes_len);
                    message_port_pub(d_psdu_out,pmt::cons(pmt::PMT_NIL,psdu_msg));
                    enter_search();
                    break;
                  }
                }
              }else{
                ncon++;
              }
            }
          break;
          default:
            throw std::runtime_error("undefined state");
          break;
        }  
      }
      consume_each (ncon);
      return 0;
    }

  } /* namespace wifi_dsss */
} /* namespace gr */
