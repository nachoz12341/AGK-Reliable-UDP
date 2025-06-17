# AGK-Reliable-UDP

A Tier 2 implementation of a Reliable UDP Listener for [AppGameKit (AGK)](https://www.appgamekit.com/), providing reliable, sequenced, and integrity-checked UDP messaging. This library is designed to overcome the unreliable nature of standard UDP by adding message sequencing, acknowledgment, retransmission, and data integrity checks.

## Features

- **Reliable UDP Messaging:** Ensures delivery of UDP packets using acknowledgments and retransmissions.
- **Message Sequencing:** Maintains correct order of messages, discarding duplicates and out-of-order packets.
- **Data Integrity:** Verifies message content using SHA-256 hashes.
- **Connection Management:** Handles connect, disconnect, and handshake messages.
- **Timeout Handling:** Automatically removes inactive connections.
- **AGK Integration:** Built to work with AGK's networking API.

## Getting Started

### Prerequisites

- C++14 compatible compiler
- [AppGameKit (AGK)](https://www.appgamekit.com/) with networking support

### Building

1. Clone this repository.
2. Add the source files (`RUDPListener.cpp`, `RUDPListener.h`, `Util.h`, etc.) to your AGK or C++ project.
3. Ensure your project links against AGK's libraries.

### Usage

#### Creating a Listener
```
#include "RUDPListener.h"

RUDPListener listener("anyip4", 12345); // Listen on all interfaces, port 12345

//In your Loop
listener.Update(); // Call regularly to process network events 
```

#### Sending a Message
```
unsigned int memblock = agk::CreateMemblockFromString("Hello, world!"); 
listener.SendMessage(connectionUUID, memblock); 
```

#### Receiving a Message
```
int memblock = listener.GetMessage(connectionUUID); //Gets every message in queue

while (memblock != -1)	
{
	// Process the memblock
	agk::DeleteMemblock(memblock); // Clean up after processing
	memblock = listener.GetMessage(connectionUUID); //Get message returns -1 when no messages are available
}
``` 

## File Overview

- `RUDPListener.h` / `RUDPListener.cpp`: Main reliable UDP listener implementation.
- `Util.h`: Utility functions (e.g., UUID generation).
- `template.cpp`: Example usage of the RUDPListener.
- `README.md`: This documentation.

## How It Works

- **Sequencing:** Each packet is assigned a sequence number. Out-of-order packets are buffered until missing packets arrive.
- **Acknowledgments:** Each received packet is acknowledged. Unacknowledged packets are resent after a timeout.
- **Integrity:** Each packet includes a SHA-256 hash of its data, which is verified on receipt.
- **Connection Management:** Handles new connections, disconnections, and timeouts automatically.

## License

MIT License. See [LICENSE](LICENSE) for details.

## Credits

Developed for use with AppGameKit (AGK) Tier 2. Inspired by common reliable UDP patterns.

---

*For questions or contributions, please open an issue or pull request.*

