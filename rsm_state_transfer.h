#ifndef rsm_state_transfer_h
#define rsm_state_transfer_h

/**
 * @brief 接口，序列化和反序列化类的状态
 */
class rsm_state_transfer {
 public:
  virtual std::string marshal_state() = 0;
  virtual void unmarshal_state(std::string) = 0;
  virtual ~rsm_state_transfer() {};
};

#endif
