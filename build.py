from build.ab import export
from build.c import cxxprogram
from build.pkg import package

package(name="hidapi-libusb", package="hidapi-libusb")
package(name="fmt", package="fmt")
package(name="x11", package="x11")
package(name="xtst", package="xtst")

cxxprogram(
    name="bmdkey",
    srcs=["src/main.cc"],
    deps=["+hidapi-libusb", "+fmt", "+x11", "+xtst"],
)

export(name="all", items={"bmdkey": "+bmdkey"})
