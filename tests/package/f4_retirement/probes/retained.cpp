#include <sluice/async/future.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/stackful_io_host.hpp>
int main(){ sluice::async::Future<int> f; f.complete_with(42); auto r=f.await(); auto* policy=&sluice::async::default_wait_policy(); sluice::async::Completion<int> c; return !r || r.value()!=42 || !policy; }
