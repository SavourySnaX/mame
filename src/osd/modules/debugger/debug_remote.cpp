// license:BSD-3-Clause
// copyright-holders:Miodrag Milanovic
//============================================================
//
//  none.c - stubs for linking when NO_DEBUGGER is defined
//
//============================================================

#include "emu.h"
#include "debug_module.h"

#include "debug/debugvw.h"
#include "debug/dvdisasm.h"
#include "debug/debugcon.h"
#include "debug/debugcpu.h"
#include "debug/points.h"
#include "debug/textbuf.h"
#include "debugger.h"

#include "modules/lib/osdobj_common.h"
#include "modules/osdmodule.h"

#include "fileio.h"

#include <cinttypes>

namespace osd {

namespace {

class debug_remote : public osd_module, public debug_module
{
public:
	debug_remote() :
		osd_module(OSD_DEBUG_PROVIDER, "remote"), debug_module(),
		m_machine(nullptr),
		m_debugger_port(0),
		m_socket(OPEN_FLAG_WRITE | OPEN_FLAG_CREATE),
        m_initialized(false)
	{
	}

	virtual ~debug_remote() { }

	virtual int init(osd_interface &osd, const osd_options &options) override 
    {
        m_debugger_port = options.debugger_port();
         return 0;
    }
	virtual void exit() override { }

	virtual void init_debugger(running_machine &machine) override;
	virtual void wait_for_debugger(device_t &device, bool firststop) override;
	virtual void debugger_update() override;

private:

    unsigned char get_byte();
    int recv(u8 first);
    void send_byte(unsigned char b);
    void send_size(int size);
    void send_data(const u8 *data, int size);
    void recv_space(address_space *data,off_t offset, int size);
    void send_space(address_space *data,off_t offset, int size);
    void send_view(debug_view *view, int x, int y, int w, int h);

    void update_socket(bool running);

    running_machine *m_machine;
    device_t *main_cpu;
    device_memory_interface* main_memory;
    address_space *program_address_space;

    debug_view_disasm* m_disasm_view;
    debug_view* m_register_view;

    int m_debugger_port;
    emu_file m_socket;
    bool m_initialized;
    char buffer[65536];
};

void debug_remote::init_debugger(running_machine &machine)
{
	m_machine = &machine;
}

void debug_remote::wait_for_debugger(device_t &device, bool firststop)
{
    if (!m_initialized)
    {
        std::string socket_name = string_format("socket.localhost:%d", m_debugger_port);
        std::error_condition const filerr = m_socket.open(socket_name);
        if (filerr)
            fatalerror("remote: failed to start listening on port %d\n", m_debugger_port);
        osd_printf_info("remote: listening on port %d\n", m_debugger_port);

        main_cpu = device_interface_enumerator<cpu_device>(m_machine->root_device()).first();
        main_memory = &main_cpu->memory();
        program_address_space = &main_memory->space(AS_PROGRAM);

        auto disasm_view = m_machine->debug_view().alloc_view(DVT_DISASSEMBLY, nullptr, this);
        m_disasm_view=downcast<debug_view_disasm*>(disasm_view);
        m_disasm_view->set_expression("curpc");
        m_register_view = m_machine->debug_view().alloc_view(DVT_STATE, nullptr, this);

        m_initialized = true;
    }

    //   bool anystopped = false;
    while (m_machine->debugger().cpu().is_stopped())
    {
        osd_sleep(osd_ticks_per_second() / 1000);
        update_socket(false);
    } 

}

void debug_remote::update_socket(bool running)
{
    if (!m_socket.is_open())
    {
        return;
    }

    u8 first = 0;
    if (m_socket.read(&first,1)==0)
        return;

    int clength = recv(first);
    if (clength==0)
    {
        m_socket.close();
        std::string socket_name = string_format("socket.localhost:%d", m_debugger_port);
        auto filerrr = m_socket.open(socket_name);
        if (filerrr)
            fatalerror("remote: failed to start listening on port %d\n", m_debugger_port);
        return;
    }
    switch (buffer[0])
    {
        case '?':
            send_data((const u8*)(running?"Y":"N"), 1);
            break;
        case 'x':
            {
                text_buffer &textbuf = m_machine->debugger().console().get_console_textbuf();
                text_buffer_clear(textbuf);
                m_machine->debugger().console().execute_command(std::string_view(buffer + 1, clength - 1), false);
                uint32_t nlines = text_buffer_num_lines(textbuf);
                std::string reply;
                if (nlines != 0)
                {
                    send_size(nlines);
                    for (uint32_t i = 0; i < nlines; i++)
                    {
                        const char *line = text_buffer_get_seqnum_line(textbuf, i);
                        send_data((u8*)line, strlen(line));
                    }
                }
                else
                {
                    send_size(0);
                }
            }
            break;
        case 'm':
            {
                int64_t address, length;
                if ( sscanf(buffer+1, "%" PRIx64 ",%" PRIx64, &address, &length) != 2 )
                {
                    osd_printf_info("remote: invalid memory read request\n");
                    send_size(0);
                    break;
                }
                offs_t offset = address;
                address_space *tspace;
                if ( !main_memory->translate(program_address_space->spacenum(), device_memory_interface::TR_READ, offset, tspace) )
                {
                    osd_printf_info("remote: invalid memory read request\n");
                    send_size(0);
                    break;
                }
                auto dis = m_machine->disable_side_effects();
                send_space(tspace,offset, length);
            }
            break;
        case 'p':
            {
                int64_t address, length;
                if ( sscanf(buffer+1, "%" PRIx64 ",%" PRIx64, &address, &length) != 2 )
                {
                    osd_printf_info("remote: invalid memory read request\n");
                    send_size(0);
                    break;
                }
                offs_t offset = address;
                address_space *tspace;
                if ( !main_memory->translate(program_address_space->spacenum(), device_memory_interface::TR_READ, offset, tspace) )
                {
                    osd_printf_info("remote: invalid memory read request\n");
                    send_size(0);
                    break;
                }
                send_size(length);
                auto dis = m_machine->disable_side_effects();
                recv_space(tspace,offset, length);
                send_size(0);
            }
            break;
        case 'v':
            {
                s32 x, y, w, h;
                if (sscanf(buffer + 2, "%d,%d,%d,%d", &x, &y, &w, &h) != 4)
                {
                    osd_printf_info("remote: invalid state view request\n");
                    send_size(0);
                    break;
                }
                switch (buffer[1])
                {
                    case 'd':
                        m_disasm_view->set_expression("curpc");
                        send_view(m_disasm_view, x, y, w, h);
                        break;
                    case 's':
                        send_view(m_register_view, x, y, w, h);
                        break;
                    default:
                        send_size(0);
                        break;
                }
            }
            break;
        default:
            send_size(0);
            break;
    }

}

void debug_remote::send_view(debug_view* view, int x, int y, int w, int h)
{
    debug_view_xy vsize;
    vsize.x = w;
    vsize.y = h;
    view->set_visible_size(vsize);
    debug_view_xy vpos;
    vpos.x = x;
    vpos.y = y;
    view->set_visible_position(vpos);
    auto viewdata = view->viewdata();
    send_size(w * h * 2);
    for (int i = 0; i < w * h; i++)
    {
        m_socket.write(&viewdata->attrib, 1);
        m_socket.write(&viewdata->byte, 1);
        viewdata++;
    }
}

unsigned char debug_remote::get_byte()
{
    int len = 0;
    while (len == 0)
    {
        len = m_socket.read(buffer, 1);
        if (len==0)
            osd_sleep(osd_ticks_per_second() / 1000);
    }
    return buffer[0];
}

void debug_remote::send_byte(unsigned char b)
{
    while (m_socket.write(&b, 1) == 0)
    {
        osd_sleep(osd_ticks_per_second() / 1000);
    }
}

void debug_remote::send_size(int size)
{
    send_byte((size >> 8) & 0xff);
    send_byte(size & 0xff);
}

void debug_remote::send_data(const u8 *data, int size)
{
    send_size(size);
    while (size!=0)
    {
        int len=m_socket.write(data, size);
        if (len==0)
        {
            osd_sleep(osd_ticks_per_second() / 1000);
            continue;
        }
        size -= len;
        data += len;
    }
}

void debug_remote::send_space(address_space *data,off_t offset, int size)
{
    send_size(size);
    while (size!=0)
    {
        u8 b = data->read_byte(offset++);
        int len=m_socket.write(&b, 1);
        if (len==0)
        {
            osd_sleep(osd_ticks_per_second() / 1000);
            continue;
        }
        size -= len;
    }
}

void debug_remote::recv_space(address_space *data,off_t offset, int size)
{
    while (size!=0)
    {
        u8 b;
        int len=m_socket.read(&b, 1);
        if (len==0)
        {
            osd_sleep(osd_ticks_per_second() / 1000);
            continue;
        }
        data->write_byte(offset++, b);
        size -= len;
    }
}


int debug_remote::recv(u8 first)
{
    int length = 0;

    length = first;
    length <<= 8;
    length |= get_byte();

    buffer[length] = 0;
    int toRead = length;
    int offs = 0;
    while (toRead)
    {
        int len = m_socket.read(buffer+offs, toRead);
        if (len == 0)
        {
            osd_sleep(osd_ticks_per_second() / 1000);
            continue;
        }
        toRead -= len;
        offs += len;
    }

    return length;
}

void debug_remote::debugger_update()
{
    update_socket(true);
}

} // anonymous namespace

} // namespace osd

MODULE_DEFINITION(DEBUG_REMOTE, osd::debug_remote)

