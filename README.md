# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite


![2023-11-06-181546_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/1d735ed0-5a62-43de-bea5-42b556a8ca41)




Simple secure and blazing fast terminal image mounter/converter written in C++, it allows mounting ISOs and/or converting BIN&IMG&MDS&MDF files to ISO. Supports Tab completion on path list generation, a curtesy of readline library. 
You can add multiple mounts and/or conversion paths. All paths are mounted under /mnt/iso_* format and all conversions take place at their local source directories, script requires ROOT access to mount&unmount ISOs. 
It is recommended that directories that are to be searched should have permissions of `755` and upwards for better list generation, but if not, you can always run it as ROOT from the beginning if need be.

Added Features:
* Cached based ISO management to reduce disk thrashing.
* Sanitized shell commands for improved security.
* Supports all filenames, including special charactes and gaps and ''.
* Extra checks and error controls so that you don't erase your / or /mnt accidentally (joking).
* More robust menu experience.
* Improved selection procedures.
* Proper multithreading support added now it can utilize up to 4 cores for maximum performance.
* Faster search times and list generetions.
* Faster mounting&unmounting times, from my tests i found it can mount, up to 30 ISO files in under 5s.
* Dropped manual mode of mounting/converting, since lists are way faster and easier to manage.
* Added support for MDF conversion to ISO by utilizing mdf2iso.
* Clean codebase, just in case someone decides to contribute in the future.

Compilation requires the readline library insatlled from your distro. 
Once requirements are met just run`g++ -o "whatever_name_you_want" -O2 mounter_elite_plus.cpp -lreadline -fopenmp`.
Make it executable and it can run from anywhere.

You can also download the binaries from my releases, or if you are on arch or on an archbased distro you can install with `yay -S mounter-elite-plus`.
