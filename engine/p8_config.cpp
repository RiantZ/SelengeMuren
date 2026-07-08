#include "p8_config.h"

#include "kit/ts_helpers.h"

#include <cstdio>
#include <cstdlib>

extern "C"
{

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    char *p8_config_read_file(const tXCHAR *ip_path, size_t *op_size)
    {
        FILE  *lp_file  = nullptr;
        char  *lp_buf   = nullptr;
        long   li_pos   = 0;
        size_t lz_size  = 0;
        size_t lz_read  = 0;
        bool   lb_error = false;

        if(op_size)
        {
            *op_size = 0;
        }

        if(!ip_path)
        {
            std::fprintf(stderr, "p8_config_read_file: NULL path\n");
            lb_error = true;
            goto lbl_exit;
        }

#ifdef G_OS_WINDOWS
        // _wfopen is flagged unsafe (C4996) by MSVC; use the _s variant, which
        // sets lp_file to NULL on failure so the shared check below still works.
        if(_wfopen_s(&lp_file, ip_path, L"rb") != 0)
        {
            lp_file = nullptr;
        }
#else
        lp_file = std::fopen(ip_path, "rb");
#endif
        if(!lp_file)
        {
            std::fprintf(stderr, "p8_config_read_file: failed to open file\n");
            lb_error = true;
            goto lbl_exit;
        }

        if(std::fseek(lp_file, 0, SEEK_END) != 0)
        {
            std::fprintf(stderr, "p8_config_read_file: fseek to end failed\n");
            lb_error = true;
            goto lbl_exit;
        }

        li_pos = std::ftell(lp_file);
        if(li_pos < 0)
        {
            std::fprintf(stderr, "p8_config_read_file: ftell failed\n");
            lb_error = true;
            goto lbl_exit;
        }
        std::rewind(lp_file);

        lz_size = static_cast<size_t>(li_pos);
        lp_buf  = static_cast<char *>(std::malloc(lz_size + 1));
        if(!lp_buf)
        {
            std::fprintf(stderr, "p8_config_read_file: malloc of %zu bytes failed\n", lz_size + 1);
            lb_error = true;
            goto lbl_exit;
        }

        lz_read = std::fread(lp_buf, 1, lz_size, lp_file);
        if(lz_read != lz_size)
        {
            std::fprintf(stderr, "p8_config_read_file: short read %zu of %zu bytes\n", lz_read, lz_size);
            lb_error = true;
            goto lbl_exit;
        }

        lp_buf[lz_size] = '\0';

        if(op_size)
        {
            *op_size = lz_size;
        }

lbl_exit:
        if(lp_file)
        {
            std::fclose(lp_file);
        }

        if(lb_error && lp_buf)
        {
            std::free(lp_buf);
            lp_buf = nullptr;
        }
        return lp_buf;
    }

} // extern "C"
