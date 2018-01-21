#ifndef YEWPAR_POLICY_PRIORITYORDERED_HPP
#define YEWPAR_POLICY_PRIORITYORDERED_HPP

#include "Policy.hpp"
#include "workstealing/priorityworkqueue.hpp"

namespace Workstealing { namespace Policies {

class PriorityOrderedPolicy : public Policy {
 private:
  hpx::naming::id_type globalWorkqueue;

  using mutex_t = hpx::lcos::local::mutex;
  mutex_t mtx;

 public:
  PriorityOrderedPolicy (hpx::naming::id_type gWorkqueue) : globalWorkqueue(gWorkqueue) {};

  // Priority Ordered policy just forwards requests to the global workqueue
  hpx::util::function<void(), false> getWork() override {
    std::unique_lock<mutex_t> l(mtx);

    hpx::util::function<void(hpx::naming::id_type)> task;
    task = hpx::async<workstealing::priorityworkqueue::steal_action>(globalWorkqueue).get();
    if (task) {
      return hpx::util::bind(task, hpx::find_here());
    }
    return nullptr;
  }

  void addwork(int priority, hpx::util::function<void(hpx::naming::id_type)> task) {
    std::unique_lock<mutex_t> l(mtx);
    //hpx::apply<workstealing::priorityworkqueue::addWork_action>(globalWorkqueue, priority, task);
    hpx::async<workstealing::priorityworkqueue::addWork_action>(globalWorkqueue, priority, task).get();
  }

  hpx::future<bool> workRemaining() {
    std::unique_lock<mutex_t> l(mtx);
    return hpx::async<workstealing::priorityworkqueue::workRemaining_action>(globalWorkqueue);
  }

  // Policy initialiser
  static void setPriorityWorkqueuePolicy(hpx::naming::id_type globalWorkqueue) {
    Workstealing::Scheduler::local_policy = std::make_shared<PriorityOrderedPolicy>(globalWorkqueue);
  }
  struct setPriorityWorkqueuePolicy_act : hpx::actions::make_action<
    decltype(&PriorityOrderedPolicy::setPriorityWorkqueuePolicy),
    &PriorityOrderedPolicy::setPriorityWorkqueuePolicy,
    setPriorityWorkqueuePolicy_act>::type {};

  static void initPolicy () {
    auto globalWorkqueue = hpx::new_<workstealing::priorityworkqueue>(hpx::find_here()).get();
    hpx::wait_all(hpx::lcos::broadcast<setPriorityWorkqueuePolicy_act>(
        hpx::find_all_localities(), globalWorkqueue));
  }
};

}}

#endif
