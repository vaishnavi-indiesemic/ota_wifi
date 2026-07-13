#ifndef MQTT_CTRL_H
#define MQTT_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the MQTT client thread.
 * 
 * This function creates a thread that connects to the MQTT broker,
 * subscribes to the OTA trigger topic, and listens for the "git" command.
 * Upon receiving "git", it triggers the OTA update pipeline.
 */
void mqtt_ctrl_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CTRL_H */
