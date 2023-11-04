# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite


![2023-11-03-231927_grim](https://github.com/siyia2/mounter-elite-Plus/assets/46220960/c7fb4ba6-ccab-4672-b12a-bc03ad5e2dc5)



Simple secure and blazing fast terminal image mounter/converter written in C++, it allows mounting ISOs and/or converting BIN&IMG&MDS&MDF files to ISO. Supports Tab completion on path list generation, a curtesy of readline library. 
You can add multiple mounts and/or conversion paths. All paths are mounted under /mnt/iso_* format and all conversions take place at their local source directories, script requires ROOT access to mount&unmount ISOs. 
It is recommended that directories that are to be searched should have permissions of 750 and upwards for better list generation, but if not, you can always run it as ROOT from the beginning if need be.

Added Features:
* Sanitized shell commands for improved security.
* Supports all filenames, including special charactes and gaps.
* Extra checks and error controls so that you don't erase your / or /mnt accidentally (joking).
* More robust menu experience.
* Improved selection procedures.
* Proper multithreading support added that can utilize up to 4 cores maximum.
* Faster search times and list generetions.
* Faster mounting&unmounting times, from my tests i found it can mount, up to 30 ISO files in under 5s.
* Dropped manual mode of mounting/converting, since lists are way faster and easier to manage.
* Added support for MDS/MDF to ISO by utilizing mdf2iso.
* Clean codebase, just in case someone decides to contribute in the future.

Compilation requires the readline library insatlled from your distro. 
Once requirements are met just run`g++ -o "whatever_name_you_want" -O2 mounter_elite_plus.cpp -lreadline`.
Make it executable and it can run from anywhere.

You can also download the binaries from my releases, or if you are on arch or on an archbased distro you can install with `yay -S mounter-elite-plus`.
