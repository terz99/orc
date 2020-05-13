# ORC (OpenWrt RESTCONF)

This is a prototype implementation of RESTCONF for the OpenWrt system that utilizes the UCI configuration files as a
datastore.

## Requirements

1. Python 3 for the YANG conversion script
2. Tool for converting YANG to YIN

## Adding YANG modules

To add YANG modules for OpenWrt they have to go through some pre-processing. This is
what the `./yin2json/yin2json.py` script does.

### Annotations

Before YANG modules can be used with this implementation they have to be
annotated with the extensions provided in `/yang/openwrt-uci-extension.yang`.
An example of an annotated module is `/yang/restconf-example.yang`

### Script

1. Convert the YANG modules to be included to YIN and put them in one folder, i.e. `/yin`.
   For example with [`pyang`](https://github.com/mbj4668/pyang)
   ```console
   pyang -f yin ./yang/restconf-example.yang -p ./yang -o ./yin/restconf-example.yin 
   ```
2. Run the `main.py` script
   ```console
   python3 ./yin2json/yin2json.py -y ./yin -o ./generated ./yin/restconf-example.yin ...
   ```
   This converts the YIN files and generates a `.h` file in `./generated` that has to be included in `/src/generated/yang.h`

## Building

1. Clone this repository
2. `Docker pull mgranderath/openwrt-build`
3. `docker run -v $(pwd):/restconf mgranderath/openwrt-build`
4. The generated `.ipk` will be in the `build` folder

## Architecture

![Architecture](docs/resources/Architecture.png)

## Testing

The tests are inside the `/test` directory and are based on the Python
[Tavern Testing Framework](https://github.com/taverntesting/tavern). After
installing the framework the tests can be run using either of the
following commands:

```console
tavern-ci ./test/test_restconf.tavern.yml
# or
py.test ./test/test_restconf.tavern.yml
```

This will run integration tests that check the actual implementation. The
url where the server is located can be changed in `/test/common.yaml`.

## Build and run locally

The binary of this implementation can also be tested locally

### Testing using CLion

Make sure you run CLion in ROOT mode. To open the project in CLion, go to File -> Open and then click on the root CMakeLists.txt. Open as Project!
Once the CMake is built by CLion, you have to edit the run configuration in CLion. Go to "Edit Configurations". The target executable `restconf` should be 
found automatically by CMake, so you only need to change `Environmental variables`.

* `REQUEST_METHOD=<whatever_request_type_you_want_to_send>`
* `CONTENT_TYPE=application/yang-data+json`
* `HTTP_ACCEPT=application/yang-data+json`
* `REQUEST_URI=/cgi-bin/restconf/data/<package-module>/<path-to-target>`: See some examples below

If you are doing a `POST` or `PUT` request then you some extra environmental variables:

* `CONTENT_LENGTH=0`: this is important
* `CONTENT_FILE_PATH=<path-to-content-file>`

#### Example of `POST` request to `lmapd` UCI config file

* `REQUEST_METHOD=POST`
* `CONTENT_TYPE=application/yang-data+json`
* `HTTP_ACCEPT=application/yang-data+json`
* `REQUEST_URI=/cgi-bin/restconf/data/lmapd:lmap/tasks/task`
* `CONTENT_LENGTH=0`
* `CONTENT_FILE_PATH=<path-to-content-file>`

Content file should look something like this:

```json
{
  "task": {
    "name": "ping",
    "program": "/usr/bin/ping",
    "option": [
      {
        "id": "count",
        "name": "-c",
        "value": "10"
      }
    ]
  }
}
```

#### Example of `GET` request to `lmapd` UCI config file

* `REQUEST_METHOD=GET`
* `CONTENT_TYPE=application/yang-data+json`
* `HTTP_ACCEPT=application/yang-data+json`
* `REQUEST_URI=/cgi-bin/restconf/data/lmapd:lmap`

### Testing using Linux command line tool

This feature is coming soon...