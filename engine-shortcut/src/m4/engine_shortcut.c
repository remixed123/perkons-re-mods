/* engine-shortcut, M4: hold SHUFFLE and press a voice's trigger key to open the settings
 * page of the engine that voice runs (the page a long press on the engine page opens).
 *
 * Works on the MAIN page (and the shuffle page SHUFFLE shows while held). Not with SHIFT
 * held: SHIFT + SHUFFLE + trigger plays a fill, as stock. The trigger key doesn't play its
 * voice, also when the voice's engine has no settings page (nothing happens then). Its
 * release and SHUFFLE's release are kept from the stock handling too: the release would
 * turn the voice off, and SHUFFLE's would leave the page for MAIN.
 *
 * On the framework's M4 host (patches/engine_shortcut.py):
 *   keys.handler_lit (chained)   -> pk_es_key_event
 *   es_pages.h (generated)       -> PK_ES_PAGES: each engine's settings page, 0 none
 */
#include <stdint.h>
#include "pk_host.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "es_pages.h"

#define SHUFFLE_MODE    8           /* the stock shuffle page (SHUFFLE held) */
#define EAT_SHUFFLE     0x10        /* bits 0-3: that voice's trigger release */

extern uint8_t pk_vm_settings_voice;   /* voice map: the voice an engine's page is for */

static const uint8_t pages[16] = PK_ES_PAGES;
static uint8_t eat;                 /* releases kept from the stock handling */

int PK_NEXT(pk_es_key_event)(const uint8_t *evt);

int pk_es_key_event(const uint8_t *evt)
{
    int k = evt[0], v = k - FW_KEY_TRIG(0);
    int trig = v >= 0 && v < 4;
    if (evt[1] == FW_EV_UP) {
        if (trig && (eat >> v & 1)) {
            eat &= (uint8_t)~(1u << v);
            return 1;
        }
        if (k == FW_KEY_SHUFFLE && (eat & EAT_SHUFFLE)) {
            eat &= (uint8_t)~EAT_SHUFFLE;
            return 1;
        }
    } else if (evt[1] == FW_EV_DOWN && trig && fw_key_held(FW_KEY_SHUFFLE) &&
               !fw_key_held(FW_KEY_SHIFT)) {
        int m = fw_get_mode();
        if (m == FW_MODE_MAIN || m == SHUFFLE_MODE) {
            int e = pk_vm_engine(v);
            eat |= (uint8_t)(1u << v);
            if (e >= 0 && e < 16 && pages[e]) {
                pk_vm_settings_voice = (uint8_t)v;
                fw_set_mode(pages[e]);
                eat |= EAT_SHUFFLE;
            }
            return 1;
        }
    }
    return PK_NEXT(pk_es_key_event)(evt);
}
