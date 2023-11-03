# mounter-elite-Plus
Port of mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

Simple asf terminal tool written in C++, that allows you to mount ISOs and/or to convert BIN/IMG files to ISO. Supports Tab completion on path list generation, a curtesy of readline library. 
You can add multiple mounts and/or conversion paths. All paths are mounted under /mnt/iso_* format and all conversions take place at their local source directories, script requires ROOT access to mount&unmount ISOs. 
It is recommended that search directories have permissions of 755 and upwards for better list generations, but if not, you can always run it as ROOT from the beginning.

Extra Added Features:
-Sanitisation support added to shell commands to improve security.
-Supports all filepaths including special charactes and gaps.
-Extra checks and error controls so that you don't erase your / or /mnt accidentally (joking).
-More robust menu experience.
-Improved selection procedures.
-Proper multithreading support can utilize up to 4 cores maximum.
-Faster search times and list generetions.
-Faster mounting&unmounting times, from my tests i found it can mount up to 30 ISO files in under 5s.
-Dropped manual mode of mounting/converting since lists are way faster and easier to manage.
-Added support for MDS/MDF to ISO by utilizing mdf2iso.
-Clean codebase, just in case someone decides to contribute in the future.


