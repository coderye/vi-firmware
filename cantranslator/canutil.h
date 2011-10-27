#ifndef _CANUTIL_H_
#define _CANUTIL_H_

#include "WProgram.h"
#include "chipKITCAN.h"
#include "bitfield.h"

/* Public: A CAN transceiver message filter mask.
 *
 * number - the ID of this mask, e.g. 0, 1, 2, 3. This is neccessary to link
 *     filters with the masks they match.
 * value - the value of the mask, e.g. 0x7ff.
 */
struct CanFilterMask {
    int number;
    int value;
};

/* Public: A CAN transceiver message filter.
 *
 * number - the ID of this filter, e.g. 0, 1, 2.
 * value - the filter's value.
 * channel - the CAN channel this filter should be applied to.
 * maskNumber - the ID of the mask this filter should be paired with.
 */
struct CanFilter {
    int number;
    int value;
    int channel;
    int maskNumber;
};

/* Public: A CAN signal to decode from the bus and output over USB.
 *
 * id          - the ID of the signal on the bus.
 * genericName - the name of the signal to be output over USB.
 * bitPosition - the starting bit of the signal in its CAN message.
 * bitSize     - the width of the bit field in the CAN message.
 * transform   - true if the singal's value should be transformed by the
 *               spcified factor and offset.
 *               TODO this may be redundant if we specify the factor and offset
 *               intelligently.
 * factor      - the final value will be multiplied by this factor if transform
 *               is true.
 * offset      - the final value will be added to this offset if transform is
 *               true.
 */
struct CanSignal {
    int id;
    char* genericName;
    int bitPosition;
    int bitSize;
    bool transform;
    float factor;
    float offset;
};

/* Public: Initializes message filter masks and filters on the CAN controller.
 *
 * canMod - a pointer to an initialized CAN module class.
 * filterMasks - an array of the filter masks to initialize.
 * filters - an array of filters to initialize.
 */
void configureFilters(CAN *canMod, CanFilterMask* filterMasks,
    CanFilter* filters);

/* Public: Parses a CAN signal from a CAN message, applies required
 *         transforations and sends the result over USB.
 *
 * data - the raw bytes of the CAN message that contains the signal.
 * signal - the details of the signal to decode and forward.
 *
 * TODO this should return the final value instead of also calling sendSignal
 */
void decodeCanSignal(CanSignal* signal, uint8_t* data);

/* Public: Constructs a JSON version of the translated CAN signal and sends over
 *         USB.
 *
 * signal - the CAN signal this value is an instance of.
 * value  - the final, translated value for the signal.
 *
 * Examples
 *
 *  sendSignal(SIGNALS[2], 42);
 *
 * TODO this should just build JSON and return it
 */
void sendSignal(CanSignal* signal, float value);

#endif // _CANUTIL_H_
