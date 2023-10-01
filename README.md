# LAB_Reliable-UDP-Transfer
## Handshake
Using TCP to send to file name to the receiver and establish connection
## Transfer
Using UDP to send data, and record the missing packets within one sending window. Then using TCP to send the missing packets back to sender
## Resend
Sender check the the missing packets in the window and resend them
## Timeout 
Initialize connection set to five times
Sending set to time approximately larger than RTT 
