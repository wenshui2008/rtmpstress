# rtmpstress - A RTMP Media Server stress tool.
Stress tester for RTMP media servers,this tool is based on librtmp,It uses multithreading for multiple concurrent connections.
This is a very useful tool for RTMP media server developers.

### Usage
rtmpstress -c <num> -i <url>

-c: specify the number of concurrent threads
-i: specify the url of rtmp stream to test
e.g.
rtmpstress -c 500 -i rtmp://xxx.xxx.xxx.xxx/live/stream

### Build

git clone https://github.com/wenshui2008/rtmpstress.git

open the project file rtmpstress.sln with Visual C++ 2010 or above. click build menu to build this lite tool.


