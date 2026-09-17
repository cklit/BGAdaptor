# BGAdaptor
The BGAdaptor connects a vintage Data Link source (**Beogram/Beogram CD/Beocord**) to a
modern Bang & Olufsen product, turning it into a fully integrated source:
- Select Line-in and the Data Link source starts playing
- Switch source and it stops
- Send the product to standby and the Data Link source turns off.

Your Data Link source behaves like a native source — controllable from the product itself, the B&O
app, a Beoremote One or a Beoremote Halo (next and previous is unfortunately not possible from the B&O app).

Under the hood, an ESP32 microcontroller monitors your B&O product over the network and translates its state into Data Link
commands. See more info below.

<img src="/images/beogram_adaptor.jpeg" width="250px">


## General user guide can be found on the [BGAdaptor Wiki page](https://github.com/cklit/BGAdaptor/wiki)<br><br>


---


# How does it work?
Physically, the BGAdaptor is a female 7-pin DIN to male 3.5mm jack
adaptor with a small piece of electronics built in. The Data Link source plugs into
the DIN end, and a jack cable is connected between the adaptor and the Line-in input of your network connected Bang & Olufsen product.

Inside the BGAdaptor, an ESP32 board is connected to the Data Link source's data pins. The ESP32 connects to your WiFi network and listens to the event stream
from your Bang & Olufsen product — similar to how the B&O app receives feedback from it. This is how the adaptor knows which source is active, and
when to send play, stop and standby commands to the Data Link source.

Communication over Data Link is bidirectional: the adaptor sends
commands, but also receives status back from the source, such as playing
state and, for CD players, the current track number.
_Please note that record players do not report if the needle is lifted. Only Playing and Stopped._

The ESP32 is powered by a separate USB power supply or an available USB port of your Bang & Olufsen product (if applicable).

**Principle (analogue audio or digital audio):**

<img src="/images/connection_analogue_connection.png" width="50%"><img src="/images/connection_digital_connection.png" width="50%">


---


# Technical details


# Hardware
Since the Data Link bus is running on 5V and an ESP32 accepts 3.3V on the GPIO pins, we need to add a little hardware. Also, Data Link is sending and receiving on the same wire, so we needed to do some trickery to get communication in both directions.

I have built my prototype using:
- 1x Lolin S3 Mini (https://www.aliexpress.com/item/1005005449219195.html)
- 1x LM358N op-amp
- 1x EL817 optocoupler
- 1x 330ohm resistor
- 1x 1K resistor
- 2x 10K resistors
- 1x female DIN7 (or 8) plug
- 1x stereo jack connector

Diagram:

![Diagram](/schematics/breadboard_diagram.png)


