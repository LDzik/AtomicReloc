# AtomicReloc

A modern version of SteamMover built in a day using Antigravity 2.0 and Gemini 3.5.

I made this because Antigravity 2.0 forces an install on the C drive. The script itself is robust—it uses atomic locking, process checks, logging, and restoration points to safely move files and create directory junctions.

**Note on App Compatibility:** While the mover is safe, some external apps just hate being moved. For example, moving Antigravity works perfectly, but moving the Oculus app will break it. Use discretion based on what you are moving.
