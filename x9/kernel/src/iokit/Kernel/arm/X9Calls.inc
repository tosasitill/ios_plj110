/* iOS20H392 VOICE bridge staging. Wire translation lives in the shared codec
 * and is tested against the original formatters / MD consumers. No call
 * state is invented here. Do not add service9 to X9Baseband's announcement
 * until Android initialization, dial/status/SIM routing and voice audio are
 * reconciled. There is deliberately NO bootarg to override this boundary.
 * x9cw_dial now translates ordinary VOICE20 digits to the captured SIM2
 * profile only when a caller explicitly supplies that verified context;
 * 0902 does not supply a CallIdentifier, so no QMI success is fabricated.
 * A verified single-call mapping is required before x9cw_action can be
 * submitted. Current code has no profile/state producer and sends no
 * MIPC call request/registration/approval. Unexpected direct VOICE requests
 * get a real unsupported result, never a success bootstrap stub. */
#include "../../../bsd/dev/x9_call_wire.h"
static bool x9c_request(uint32_t local,uint32_t port,const uint8_t *req,unsigned len){
 if(port!=X9BB_PORT_BASE+9)return false;
 x9q_packet p(req,X9Q_UNSUPPORTED);p.send(local,port);(void)len;return true;
}
