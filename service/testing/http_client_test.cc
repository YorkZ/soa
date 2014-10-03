#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include <memory>
#include <string>
#include <tuple>
#include <boost/test/unit_test.hpp>

#include "jml/utils/testing/watchdog.h"
#include "soa/service/rest_proxy.h"
#include "soa/service/http_client.h"
#include "soa/utils/print_utils.h"

#include "test_http_services.h"


using namespace std;
using namespace Datacratic;


/* helpers functions used in tests */
namespace {

typedef tuple<int, int, string> ClientResponse;

#define CALL_MEMBER_FN(object, pointer)  (object.*(pointer))

/* sync request helpers */
template<typename Func>
ClientResponse
doRequest(const string & baseUrl, const string & resource,
          Func func,
          const RestParams & queryParams, const RestParams & headers,
          int timeout = -1)
{
    ClientResponse response;

    HttpClient client(baseUrl);
    client.start();

    int done(false);
    auto onResponse = [&] (const HttpRequest & rq,
                           int error,
                           int status,
                           string && headers,
                           string && body) {
        int & code = get<1>(response);
        code = status;
        string & body_ = get<2>(response);
        body_ = move(body);
        int & errorCode = get<0>(response);
        errorCode = error;
        done = true;
        ML::futex_wake(done);
    };
    auto cbs = make_shared<HttpClientSimpleCallbacks>(onResponse);

    CALL_MEMBER_FN(client, func)(resource, cbs, queryParams, headers,
                                 timeout);

    while (!done) {
        int oldDone = done;
        ML::futex_wait(done, oldDone);
    }

    client.shutdown();

    return response;
}

ClientResponse
doGetRequest(const string & baseUrl, const string & resource,
             const RestParams & queryParams = RestParams(),
             const RestParams & headers = RestParams(),
             int timeout = -1)
{
    return doRequest(baseUrl, resource, &HttpClient::get,
                     queryParams, headers, timeout);
}

ClientResponse
doDeleteRequest(const string & baseUrl, const string & resource,
                const RestParams & queryParams = RestParams(),
                const RestParams & headers = RestParams(),
                int timeout = -1)
{
    return doRequest(baseUrl, resource, &HttpClient::del,
                     queryParams, headers, timeout);
}

ClientResponse
doUploadRequest(bool isPut,
                const string & baseUrl, const string & resource,
                const string & body, const string & type)
{
    ClientResponse response;

    HttpClient client(baseUrl);
    client.start();

    int done(false);
    auto onResponse = [&] (const HttpRequest & rq,
                           int error,
                           int status,
                           string && headers,
                           string && body) {
        int & code = get<1>(response);
        code = status;
        string & body_ = get<2>(response);
        body_ = move(body);
        int & errorCode = get<0>(response);
        errorCode = error;
        done = true;
        ML::futex_wake(done);
    };

    auto cbs = make_shared<HttpClientSimpleCallbacks>(onResponse);
    MimeContent content(body, type);
    if (isPut) {
        client.put(resource, cbs, content);
    }
    else {
        client.post(resource, cbs, content);
    }

    while (!done) {
        int oldDone = done;
        ML::futex_wait(done, oldDone);
    }
    
    return response;
}

}

#if 1
BOOST_AUTO_TEST_CASE( test_http_client_get )
{
    cerr << "client_get\n";
    ML::Watchdog watchdog(10);
    auto proxies = make_shared<ServiceProxies>();
    HttpGetService service(proxies);

    service.addResponse("GET", "/coucou", 200, "coucou");
    service.start();

    service.waitListening();

#if 0
    /* request to bad ip
       Note: if the ip resolution timeout is very high on the router, the
       Watchdog timeout might trigger first */
    {
        ::fprintf(stderr, "request to bad ip\n");
        string baseUrl("http://123.234.12.23");
        auto resp = doGetRequest(baseUrl, "/");
        BOOST_CHECK_EQUAL(get<0>(resp),
                          TcpConnectionCode::COULD_NOT_CONNECT);
        BOOST_CHECK_EQUAL(get<1>(resp), 0);
    }
#endif

#if 0
    /* request to bad hostname
       Note: will fail when the name service returns a "default" value for all
       non resolved hosts */
    {
        ::fprintf(stderr, "request to bad hostname\n");
        string baseUrl("http://somewhere.lost");
        auto resp = doGetRequest(baseUrl, "/");
        BOOST_CHECK_EQUAL(get<0>(resp),
                          TcpConnectionCode::HOST_UNKNOWN);
        BOOST_CHECK_EQUAL(get<1>(resp), 0);
    }
#endif

    /* request with timeout */
    {
        ::fprintf(stderr, "request with timeout\n");
        string baseUrl("http://127.0.0.1:" + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/timeout", {}, {}, 1);
        BOOST_CHECK_EQUAL(get<0>(resp),
                          TcpConnectionCode::Timeout);
        BOOST_CHECK_EQUAL(get<1>(resp), 0);
    }

    /* request connection close  */
    {
        ::fprintf(stderr, "testing behaviour with connection: close\n");
        string baseUrl("http://127.0.0.1:" + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/connection-close");
        BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
        BOOST_CHECK_EQUAL(get<1>(resp), 204);
    }

    /* request to /nothing -> 404 */
    {
        ::fprintf(stderr, "request with 404\n");
        string baseUrl("http://127.0.0.1:"
                       + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/nothing");
        BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
        BOOST_CHECK_EQUAL(get<1>(resp), 404);
    }

    /* request to /coucou -> 200 + "coucou" */
    {
        ::fprintf(stderr, "request with 200\n");
        string baseUrl("http://127.0.0.1:"
                       + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/coucou");
        BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
        BOOST_CHECK_EQUAL(get<1>(resp), 200);
        BOOST_CHECK_EQUAL(get<2>(resp), "coucou");
    }

    /* headers and cookies */
    {
        string baseUrl("http://127.0.0.1:" + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/headers", {},
                                 {{"someheader", "somevalue"}});
        Json::Value expBody;
        expBody["accept"] = "*/*";
        expBody["host"] = baseUrl.substr(7);
        expBody["someheader"] = "somevalue";
        Json::Value jsonBody = Json::parse(get<2>(resp));
        BOOST_CHECK_EQUAL(jsonBody, expBody);
    }

    /* query-params */
    {
        string baseUrl("http://127.0.0.1:" + to_string(service.port()));
        auto resp = doGetRequest(baseUrl, "/query-params",
                                 {{"value", "hello"}});
        string body = get<2>(resp);
        BOOST_CHECK_EQUAL(body, "?value=hello");
    }
}
#endif

#if 1
BOOST_AUTO_TEST_CASE( test_http_client_post )
{
    cerr << "client_post\n";
    ML::Watchdog watchdog(10);
    auto proxies = make_shared<ServiceProxies>();
    HttpUploadService service(proxies);
    service.start();

    /* request to /coucou -> 200 + "coucou" */
    {
        string baseUrl("http://127.0.0.1:"
                       + to_string(service.port()));
        auto resp = doUploadRequest(false, baseUrl, "/post-test",
                                    "post body", "application/x-nothing");
        BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
        BOOST_CHECK_EQUAL(get<1>(resp), 200);
        Json::Value jsonBody = Json::parse(get<2>(resp));
        BOOST_CHECK_EQUAL(jsonBody["verb"], "POST");
        BOOST_CHECK_EQUAL(jsonBody["payload"], "post body");
        BOOST_CHECK_EQUAL(jsonBody["type"], "application/x-nothing");
    }
}
#endif

#if 1
BOOST_AUTO_TEST_CASE( test_http_client_put )
{
    cerr << "client_put\n";
    ML::Watchdog watchdog(10);
    auto proxies = make_shared<ServiceProxies>();
    HttpUploadService service(proxies);
    service.start();

    string baseUrl("http://127.0.0.1:"
                   + to_string(service.port()));
    string bigBody;
    for (int i = 0; i < 65535; i++) {
        bigBody += "this is one big body,";
    }
    auto resp = doUploadRequest(true, baseUrl, "/put-test",
                                bigBody, "application/x-nothing");
    BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
    BOOST_CHECK_EQUAL(get<1>(resp), 200);
    Json::Value jsonBody = Json::parse(get<2>(resp));
    BOOST_CHECK_EQUAL(jsonBody["verb"], "PUT");
    BOOST_CHECK_EQUAL(jsonBody["payload"], bigBody);
    BOOST_CHECK_EQUAL(jsonBody["type"], "application/x-nothing");
}
#endif

#if 1
BOOST_AUTO_TEST_CASE( http_test_client_delete )
{
    cerr << "client_delete" << endl;
    ML::Watchdog watchdog(10);

    auto proxies = make_shared<ServiceProxies>();
    HttpGetService service(proxies);

    service.addResponse("DELETE", "/deleteMe", 200, "Deleted");
    service.start();

    string baseUrl("http://127.0.0.1:" + to_string(service.port()));
    auto resp = doDeleteRequest(baseUrl, "/deleteMe", {}, {}, 1);

    BOOST_CHECK_EQUAL(get<0>(resp), TcpConnectionCode::Success);
    BOOST_CHECK_EQUAL(get<1>(resp), 200);
}
#endif

#if 1
BOOST_AUTO_TEST_CASE( test_http_client_put_multi )
{
    cerr << "client_put_multi\n";
    auto proxies = make_shared<ServiceProxies>();
    HttpUploadService service(proxies);
    service.start();

    string baseUrl("http://127.0.0.1:"
                   + to_string(service.port()));

    HttpClient client(baseUrl);
    client.start();

    size_t maxRequests(500);
    int done(0);

    auto makeBody = [&] (size_t i) {
        int multiplier = (i < maxRequests / 2) ? -2 : 2;
        size_t bodySize = 2000 + multiplier * i;
        string body = ML::format("%.4x", bodySize);
        size_t rndSize = bodySize - body.size();
        body += randomString(rndSize);

        return body;
    };

    for (size_t i = 0; i < maxRequests; i++) {
        auto sendBody = makeBody(i);
        auto onResponse = [&, sendBody] (const HttpRequest & rq,
                                         int error,
                                         int status,
                                         string && headers,
                                         string && body) {
            BOOST_CHECK_EQUAL(error, TcpConnectionCode::Success);
            BOOST_CHECK_EQUAL(status, 200);
            Json::Value jsonBody = Json::parse(body);
            BOOST_CHECK_EQUAL(jsonBody["verb"], "PUT");
            BOOST_CHECK_EQUAL(jsonBody["payload"], sendBody);
            BOOST_CHECK_EQUAL(jsonBody["type"], "text/plain");
            done++;
            if (done == maxRequests) {
                ML::futex_wake(done);
            }
        };

        auto cbs = make_shared<HttpClientSimpleCallbacks>(onResponse);
        MimeContent content(sendBody, "text/plain");
        while (!client.put(baseUrl, cbs, content)) {
            ML::sleep(0.2);
        }
    };

    while (done < maxRequests) {
        int oldDone = done;
        ML::futex_wait(done, oldDone);
    }
}
#endif

#if 1
/* Ensures that all requests are correctly performed under load, including
   when "Connection: close" is encountered once in a while.
   Not a performance test. */
BOOST_AUTO_TEST_CASE( test_http_client_stress_test )
{
    cerr << "stress_test\n";
    // const int mask = 0x3ff; /* mask to use for displaying counts */
    // ML::Watchdog watchdog(300);
    auto proxies = make_shared<ServiceProxies>();
    auto doStressTest = [&] (int numParallel) {
        ::fprintf(stderr, "stress test with %d parallel connections\n",
                  numParallel);

        HttpGetService service(proxies);
        service.start();
        service.waitListening();

        string baseUrl("http://127.0.0.1:"
                       + to_string(service.port()));

        HttpClient client(baseUrl, numParallel);
        client.start();

        int maxReqs(30000), numReqs(0), missedReqs(0);
        int numResponses(0);

        auto onDone = [&] (const HttpRequest & rq,
                           int errorCode, int status,
                           string && headers, string && body) {
            numResponses++;

            BOOST_CHECK_EQUAL(errorCode, 0);
            BOOST_CHECK_EQUAL(status, 200);

            int bodyNbr;
            try {
                bodyNbr = stoi(body);
            }
            catch (...) {
                ::fprintf(stderr, "exception when parsing body: %s\n",
                          body.c_str());
                throw;
            }

            int lowerLimit = std::max(0, (numResponses - numParallel));
            int upperLimit = std::min(maxReqs, (numResponses + numParallel));
            if (bodyNbr < lowerLimit || bodyNbr > upperLimit) {
                throw ML::Exception("number of returned server requests "
                                    " is anomalous: %d is out of range"
                                    " [%d,*%d,%d]",
                                    bodyNbr, lowerLimit,
                                    numResponses, upperLimit);
            }

            if (numResponses == numReqs) {
                ML::futex_wake(numResponses);
            }
        };
        auto cbs = make_shared<HttpClientSimpleCallbacks>(onDone);

        while (numReqs < maxReqs) {
            const char * url = "/counter";
            if (client.get(url, cbs)) {
                numReqs++;
                // if ((numReqs & mask) == 0 || numReqs == maxReqs) {
                //     ::fprintf(stderr, "performed %d requests\n", numReqs);
                // }
            }
            else {
                missedReqs++;
            }
        }

        ::fprintf(stderr, "all requests performed, awaiting responses...\n");
        while (numResponses < maxReqs) {
            int old(numResponses);
            ML::futex_wait(numResponses, old);
        }
        ::fprintf(stderr, "performed %d requests; missed: %d\n",
                  maxReqs, missedReqs);
    };

    doStressTest(1);
    doStressTest(8);
    doStressTest(128);
}
#endif

#if 1
/* Ensure that an infinite number of requests can be queued when queue size is
 * 0, even from within callbacks. */
BOOST_AUTO_TEST_CASE( test_http_client_unlimited_queue )
{
    static const int maxLevel(4);

    ML::Watchdog watchdog(30);
    auto proxies = make_shared<ServiceProxies>();

    HttpGetService service(proxies);
    service.addResponse("GET", "/", 200, "coucou");
    service.start();
    service.waitListening();

    MessageLoop loop;
    loop.start();

    string baseUrl("http://127.0.0.1:"
                   + to_string(service.port()));

    auto client = make_shared<HttpClient>(baseUrl, 4, 0);
    loop.addSource("client", client);
    client->waitConnectionState(AsyncEventSource::CONNECTED);

    atomic<int> pending(0);
    int done(0);

    function<void(int)> doGet = [&] (int level) {
        pending++;
        auto onDone = [&,level] (const HttpRequest & rq,
                                 int errorCode, int status,
                                 string && headers, string && body) {
            if (level < maxLevel) {
                for (int i = 0; i < 10; i++) {
                    doGet(level+1);
                }
            }
            pending--;
            done++;
        };
        auto cbs = make_shared<HttpClientSimpleCallbacks>(onDone);
        client->get("/", cbs);
    };

    doGet(0);

    while (pending > 0) {
        ML::sleep(1);
        cerr << "requests done: " + to_string(done) + "\n";
    }

    loop.removeSource(client.get());
    client->waitConnectionState(AsyncEventSource::DISCONNECTED);
}
#endif

BOOST_AUTO_TEST_CASE( test_http_client_expect_100_continue )
{
    ML::Watchdog watchdog(10);
    cerr << "client_expect_100_continue" << endl;

    auto proxies = make_shared<ServiceProxies>();

    HttpUploadService service(proxies);
    service.start();

    string baseUrl("http://127.0.0.1:"
                   + to_string(service.port()));

    auto client = make_shared<HttpClient>(baseUrl);
    client->debug(true);
    client->sendExpect100Continue(true);

    MessageLoop loop;
    loop.addSource("HttpClient", client);
    loop.start();

    HttpHeader sentHeaders;

    auto debugCallback = [&](const HttpRequest&, curl_infotype info,
                             char *buffer, size_t size)
    {
        if (info == CURLINFO_HEADER_OUT) {
            const string headers(buffer, size);
            sentHeaders.parse(headers);
        }
    };

    {
        int done(false);
        auto callbacks = std::make_shared<HttpClientSimpleCallbacks>(
                [&](const HttpRequest&, HttpClientError error,
                            int statusCode, std::string&&, std::string&&)
        {
            BOOST_CHECK_EQUAL(error, HttpClientError::None);
            BOOST_CHECK_EQUAL(statusCode, 200);
            done = true;
            ML::futex_wake(done);
        });

        callbacks->useDebug(debugCallback);

        const std::string& smallPayload = randomString(20);
        HttpRequest::Content content(smallPayload, "application/x-nothing");
        client->post("/post-test", callbacks, content);

        while (!done) {
            ML::futex_wait(done, false);
        }

        BOOST_CHECK_EQUAL(sentHeaders.tryGetHeader("expect"), "");
        
    }

    {
        int done(false);
        auto callbacks = std::make_shared<HttpClientSimpleCallbacks>(
                [&](const HttpRequest&, HttpClientError error,
                            int statusCode, std::string&&, std::string&&)
        {
            BOOST_CHECK_EQUAL(error, HttpClientError::None);
            done = true;
            ML::futex_wake(done);
        });

        callbacks->useDebug(debugCallback);

        const std::string& bigPayload = randomString(2024);
        HttpRequest::Content content(bigPayload, "application/x-nothing");
        client->post("/post-test", callbacks, content);

        while (!done) {
            ML::futex_wait(done, false);
        }

        BOOST_CHECK_EQUAL(sentHeaders.tryGetHeader("expect"), "100-continue");
    }

    client->sendExpect100Continue(false);

    {
        int done(false);
        auto callbacks = std::make_shared<HttpClientSimpleCallbacks>(
                [&](const HttpRequest&, HttpClientError error,
                            int statusCode, std::string&&, std::string&&)
        {
            BOOST_CHECK_EQUAL(error, HttpClientError::None);
            done = true;
            ML::futex_wake(done);
        });

        callbacks->useDebug(debugCallback);

        const std::string& bigPayload = randomString(2024);
        HttpRequest::Content content(bigPayload, "application/x-nothing");
        client->post("/post-test", callbacks, content);

        while (!done) {
            ML::futex_wait(done, false);
        }

        BOOST_CHECK_EQUAL(sentHeaders.tryGetHeader("expect"), "");
    }


}
