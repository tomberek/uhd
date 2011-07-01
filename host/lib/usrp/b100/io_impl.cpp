//
// Copyright 2011 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "validate_subdev_spec.hpp"
#include "../../transport/super_recv_packet_handler.hpp"
#include "../../transport/super_send_packet_handler.hpp"
#include "usrp_commands.h"
#include "b100_impl.hpp"
#include "b100_regs.hpp"
#include <uhd/utils/thread_priority.hpp>
#include <uhd/transport/bounded_buffer.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/log.hpp>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;
namespace asio = boost::asio;

/***********************************************************************
 * IO Implementation Details
 **********************************************************************/
struct b100_impl::io_impl{
    io_impl(zero_copy_if::sptr data_transport, const size_t recv_width):
        data_transport(data_transport), async_msg_fifo(100/*messages deep*/)
    {
        for (size_t i = 0; i < recv_width; i++){
            typedef bounded_buffer<managed_recv_buffer::sptr> buffs_queue_type;
            _buffs_queue.push_back(new buffs_queue_type(data_transport->get_num_recv_frames()));
        }
    }

    ~io_impl(void){
        for (size_t i = 0; i < _buffs_queue.size(); i++){
            delete _buffs_queue[i];
        }
    }

    zero_copy_if::sptr data_transport;
    bounded_buffer<async_metadata_t> async_msg_fifo;
    std::vector<bounded_buffer<managed_recv_buffer::sptr> *> _buffs_queue;

    //gets buffer, determines if its the requested index,
    //and either queues the buffer or returns the buffer
    managed_recv_buffer::sptr get_recv_buff(const size_t index, const double timeout){
        while (true){
            managed_recv_buffer::sptr buff;

            //attempt to pop a buffer from the queue
            if (_buffs_queue[index]->pop_with_haste(buff)) return buff;

            //otherwise, call into the transport
            buff = data_transport->get_recv_buff(timeout);
            if (buff.get() == NULL) return buff; //timeout

            //check the stream id to know which channel
            const boost::uint32_t *vrt_hdr = buff->cast<const boost::uint32_t *>();
            const size_t rx_index = uhd::wtohx(vrt_hdr[1]) - B100_RX_SID_BASE;
            if (rx_index == index) return buff; //got expected message

            //otherwise queue and try again
            if (rx_index < _buffs_queue.size()) _buffs_queue[rx_index]->push_with_pop_on_full(buff);
            else UHD_MSG(error) << "Got a data packet with known SID " << uhd::wtohx(vrt_hdr[1]) << std::endl;
        }
    }

    sph::recv_packet_handler recv_handler;
    sph::send_packet_handler send_handler;
};

/***********************************************************************
 * Initialize internals within this file
 **********************************************************************/
void b100_impl::io_init(void){

    //setup rx otw type
    _rx_otw_type.width = 16;
    _rx_otw_type.shift = 0;
    _rx_otw_type.byteorder = uhd::otw_type_t::BO_LITTLE_ENDIAN;

    //setup tx otw type
    _tx_otw_type.width = 16;
    _tx_otw_type.shift = 0;
    _tx_otw_type.byteorder = uhd::otw_type_t::BO_LITTLE_ENDIAN;

    //TODO best place to put this?
    this->reset_gpif(6);

    //set the expected packet size in USB frames
    _fpga_ctrl->poke32(B100_REG_MISC_RX_LEN, 4);

    //create new io impl
    _io_impl = UHD_PIMPL_MAKE(io_impl, (_data_transport, _rx_dsps.size()));

    //now its safe to register the async callback
    _fpga_ctrl->set_async_cb(boost::bind(&b100_impl::handle_async_message, this, _1));

    //init some handler stuff
    _io_impl->recv_handler.set_vrt_unpacker(&vrt::if_hdr_unpack_le);
    _io_impl->recv_handler.set_converter(_rx_otw_type);
    _io_impl->send_handler.set_vrt_packer(&vrt::if_hdr_pack_le);
    _io_impl->send_handler.set_converter(_tx_otw_type);
    _io_impl->send_handler.set_max_samples_per_packet(get_max_send_samps_per_packet());
}

void b100_impl::handle_async_message(managed_recv_buffer::sptr rbuf){
    vrt::if_packet_info_t if_packet_info;
    if_packet_info.num_packet_words32 = rbuf->size()/sizeof(boost::uint32_t);
    const boost::uint32_t *vrt_hdr = rbuf->cast<const boost::uint32_t *>();
    try{
        vrt::if_hdr_unpack_le(vrt_hdr, if_packet_info);
    }
    catch(const std::exception &e){
        UHD_MSG(error) << "Error (handle_async_message): " << e.what() << std::endl;
    }

    if (if_packet_info.sid == B100_TX_ASYNC_SID and if_packet_info.packet_type != vrt::if_packet_info_t::PACKET_TYPE_DATA){
        //fill in the async metadata
        async_metadata_t metadata;
        metadata.channel = 0;
        metadata.has_time_spec = if_packet_info.has_tsi and if_packet_info.has_tsf;
        metadata.time_spec = time_spec_t(
            time_t(if_packet_info.tsi), size_t(if_packet_info.tsf), _clock_ctrl->get_fpga_clock_rate()
        );
        metadata.event_code = async_metadata_t::event_code_t(sph::get_context_code(vrt_hdr, if_packet_info));
        _io_impl->async_msg_fifo.push_with_pop_on_full(metadata);
        if (metadata.event_code &
            ( async_metadata_t::EVENT_CODE_UNDERFLOW
            | async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET)
        ) UHD_MSG(fastpath) << "U";
        else if (metadata.event_code &
            ( async_metadata_t::EVENT_CODE_SEQ_ERROR
            | async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST)
        ) UHD_MSG(fastpath) << "S";
    }
    else UHD_MSG(error) << "Unknown async packet" << std::endl;
}

void b100_impl::update_tick_rate(const double rate){
    boost::mutex::scoped_lock recv_lock = _io_impl->recv_handler.get_scoped_lock();
    _io_impl->recv_handler.set_tick_rate(rate);
    boost::mutex::scoped_lock send_lock = _io_impl->send_handler.get_scoped_lock();
    _io_impl->send_handler.set_tick_rate(rate);
}

void b100_impl::update_rx_samp_rate(const double rate){
    boost::mutex::scoped_lock recv_lock = _io_impl->recv_handler.get_scoped_lock();
    _io_impl->recv_handler.set_samp_rate(rate);
}

void b100_impl::update_tx_samp_rate(const double rate){
    boost::mutex::scoped_lock send_lock = _io_impl->send_handler.get_scoped_lock();
    _io_impl->send_handler.set_samp_rate(rate);
}

void b100_impl::update_rx_subdev_spec(const uhd::usrp::subdev_spec_t &spec){
    boost::mutex::scoped_lock recv_lock = _io_impl->recv_handler.get_scoped_lock();
    property_tree::path_type root = "/mboards/0/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "rx");

    //setup mux for this spec
    for (size_t i = 0; i < spec.size(); i++){
        //ASSUME that we dont swap the rx fe mux...
        const std::string conn = _tree->access<std::string>(root / spec[i].db_name / "rx_frontends" / spec[i].sd_name / "connection").get();
        _rx_dsps[i]->set_mux(conn);
    }

    //resize for the new occupancy
    _io_impl->recv_handler.resize(spec.size());

    //bind new callbacks for the handler
    for (size_t i = 0; i < _io_impl->recv_handler.size(); i++){
        _rx_dsps[i]->set_nsamps_per_packet(get_max_recv_samps_per_packet()); //seems to be a good place to set this
        _io_impl->recv_handler.set_xport_chan_get_buff(i, boost::bind(
            &b100_impl::io_impl::get_recv_buff, _io_impl.get(), i, _1
        ));
        _io_impl->recv_handler.set_overflow_handler(i, boost::bind(&rx_dsp_core_200::handle_overflow, _rx_dsps[i]));
    }
}

void b100_impl::update_tx_subdev_spec(const uhd::usrp::subdev_spec_t &spec){
    boost::mutex::scoped_lock send_lock = _io_impl->send_handler.get_scoped_lock();
    property_tree::path_type root = "/mboards/0/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "tx");

    //set the mux for this spec
    const std::string conn = _tree->access<std::string>(root / spec[0].db_name / "tx_frontends" / spec[0].sd_name / "connection").get();
    _tx_fe->set_mux(conn);

    //resize for the new occupancy
    _io_impl->send_handler.resize(spec.size());

    //bind new callbacks for the handler
    for (size_t i = 0; i < _io_impl->send_handler.size(); i++){
        _io_impl->send_handler.set_xport_chan_get_buff(i, boost::bind(
            &zero_copy_if::get_send_buff, _data_transport, _1
        ));
    }
}

/***********************************************************************
 * Async Data
 **********************************************************************/
bool b100_impl::recv_async_msg(
    async_metadata_t &async_metadata, double timeout
){
    boost::this_thread::disable_interruption di; //disable because the wait can throw
    return _io_impl->async_msg_fifo.pop_with_timed_wait(async_metadata, timeout);
}

/***********************************************************************
 * Send Data
 **********************************************************************/
size_t b100_impl::get_max_send_samps_per_packet(void) const {
    static const size_t hdr_size = 0
        + vrt::max_if_hdr_words32*sizeof(boost::uint32_t)
        - sizeof(vrt::if_packet_info_t().cid) //no class id ever used
    ;
    static const size_t bpp = 2048 - hdr_size;
    return bpp / _tx_otw_type.get_sample_size();
}

size_t b100_impl::send(
    const send_buffs_type &buffs, size_t nsamps_per_buff,
    const tx_metadata_t &metadata, const io_type_t &io_type,
    send_mode_t send_mode, double timeout
){
    return _io_impl->send_handler.send(
        buffs, nsamps_per_buff,
        metadata, io_type,
        send_mode, timeout
    );
}

/***********************************************************************
 * Receive Data
 **********************************************************************/
size_t b100_impl::get_max_recv_samps_per_packet(void) const {
    static const size_t hdr_size = 0
        + vrt::max_if_hdr_words32*sizeof(boost::uint32_t)
        + sizeof(vrt::if_packet_info_t().tlr) //forced to have trailer
        - sizeof(vrt::if_packet_info_t().cid) //no class id ever used
    ;
    size_t bpp = 2048 - hdr_size; //limited by FPGA pkt buffer size
    return bpp/_rx_otw_type.get_sample_size();
}

size_t b100_impl::recv(
    const recv_buffs_type &buffs, size_t nsamps_per_buff,
    rx_metadata_t &metadata, const io_type_t &io_type,
    recv_mode_t recv_mode, double timeout
){
    return _io_impl->recv_handler.recv(
        buffs, nsamps_per_buff,
        metadata, io_type,
        recv_mode, timeout
    );
}
